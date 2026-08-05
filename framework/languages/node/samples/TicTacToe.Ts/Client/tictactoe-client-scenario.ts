import {
  GameMarks,
  GameStatus,
  LeaveGameMsg,
  ObserveMilestoneReq,
  PacketNames,
  authenticateReq,
  createGameHttpReq,
  joinGameReq,
  placeMarkStreamReq
} from '../Shared/Contracts/messages';
import { BrowserHttpClientFactory } from '../../browser-client-runtime';
import * as connector from '@zlink-systems/stream-connector';
import type {
  AuthenticateRes,
  CreateGameHttpRes,
  GameState,
  GameStateNotify,
  JoinGameRes,
  ObserveMilestoneRes,
  PlaceMarkRes,
  PlayerJoinedNotify,
  WinMilestoneNotify
} from '../Shared/Contracts/messages';
import type { ZlinkStreamConnector, ZlinkStreamMessage } from '@zlink-systems/stream-connector';

class TicTacToeClientScenario {
  async run(apiHttpEndpoint: string, signal?: AbortSignal): Promise<void> {
    const api = BrowserHttpClientFactory.create(apiHttpEndpoint).build();
    let game: CreateGameHttpRes;
    try {
      // 1. Create the room through API.
      game = await api
        .post('/games')
        .body(createGameHttpReq('match-ready'))
        .fetch<CreateGameHttpRes>();
    } finally {
      await api.close();
    }

    connector.zlinkStreamAssert.ensure(game.gameName === 'match-ready', 'Sample scenario assertion failed.');
    connector.zlinkStreamAssert.ensure(game.roomId.length > 0, 'Sample scenario assertion failed.');
    connector.zlinkStreamAssert.ensure(game.playEndpoints.length >= 2, 'Sample scenario assertion failed.');
    connector.zlinkStreamAssert.ensure(new Set(game.playEndpoints).size === game.playEndpoints.length, 'Sample scenario assertion failed.');
    connector.zlinkStreamAssert.ensure(game.playNodes.length === game.playEndpoints.length, 'Sample scenario assertion failed.');
    connector.zlinkStreamAssert.ensure(game.playNodes.every((node) => game.playEndpoints.includes(node.streamEndpoint)), 'Sample scenario assertion failed.');
    connector.zlinkStreamAssert.ensure(game.requiredLevel === 3, 'Sample scenario assertion failed.');
    connector.zlinkStreamAssert.ensure(game.playEndpoints.length > 0, 'Sample scenario assertion failed.');
    const hostPlayEndpoint = game.playEndpoints[0];
    const guestPlayEndpoint = game.playEndpoints.find((endpoint) => endpoint !== hostPlayEndpoint);
    connector.zlinkStreamAssert.ensure(guestPlayEndpoint !== undefined, 'Sample scenario assertion failed.');
    const observerPlayEndpoint = guestPlayEndpoint as string;
    const observerPlayNode = game.playNodes.find((node) => node.streamEndpoint === observerPlayEndpoint);
    connector.zlinkStreamAssert.ensure(observerPlayNode !== undefined, 'Sample scenario assertion failed.');

    const observedClients = new Set<string>();
    const client1 = createPlayerClient(hostPlayEndpoint, 'host', observedClients);
    const client2 = createPlayerClient(observerPlayEndpoint, 'guest', observedClients);
    const observer = createPlayerClient(observerPlayEndpoint, 'observer', observedClients);

    try {
      // 2. Host, guest, and observer connect directly to Play stream endpoints from the API response.
      await client1.connect(signal);
      const client1Auth = await client1.request(authenticateReq('player-x')).submit<AuthenticateRes>(signal);
      connector.zlinkStreamAssert.ensure(client1Auth.player.actorId === 'player-x', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1Auth.player.displayName.length > 0, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1Auth.player.level >= game.requiredLevel, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1Auth.player.wins === 99, 'Sample scenario assertion failed.');

      await client2.connect(signal);
      const client2Auth = await client2.request(authenticateReq('player-o')).submit<AuthenticateRes>(signal);
      connector.zlinkStreamAssert.ensure(client2Auth.player.actorId === 'player-o', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client2Auth.player.displayName.length > 0, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client2Auth.player.level >= game.requiredLevel, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client2Auth.player.wins >= 0, 'Sample scenario assertion failed.');

      await observer.connect(signal);
      const observerAuth = await observer.request(authenticateReq('observer')).submit<AuthenticateRes>(signal);
      connector.zlinkStreamAssert.ensure(observerAuth.player.actorId === 'observer', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(observerAuth.player.displayName.length > 0, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(observerAuth.player.level >= game.requiredLevel, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(observerAuth.player.wins === 0, 'Sample scenario assertion failed.');
      const observerSubscription = await observer
        .request(new ObserveMilestoneReq())
        .submit<ObserveMilestoneRes>(signal);
      connector.zlinkStreamAssert.ensure(observerSubscription.subscribed, 'Sample scenario assertion failed.');
      console.log('observer-subscription=verified');

      // 3. Host joins by explicit RoomId
      const client1JoinedState = client1
        .waitFor<GameStateNotify>(PacketNames.gameStateNotify)
        .where((message) =>
          message.payload.state.roomId === game.roomId &&
          message.payload.state.xActorId === client1Auth.player.actorId)
        .submit(signal);
      const [client1Join, client1State] = await Promise.all([
        client1.request(joinGameReq(game.roomId)).submit<JoinGameRes>(signal),
        client1JoinedState,
        client1.expectNone<PlayerJoinedNotify>(PacketNames.playerJoinedNotify).within(250).run(signal)
      ]);
      connector.zlinkStreamAssert.ensure(stateOf(client1Join).roomId === game.roomId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1State.payload.state.status === GameStatus.WaitingForPlayers, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1State.payload.state.xActorId === client1Auth.player.actorId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1State.payload.state.oActorId === null, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1State.payload.state.board === '.........', 'Sample scenario assertion failed.');
      const client1SawClient2Join = client1
        .waitFor<PlayerJoinedNotify>(PacketNames.playerJoinedNotify)
        .where((message) =>
          message.payload.roomId === game.roomId
          && message.payload.actorId === client2Auth.player.actorId)
        .submit(signal);
      // 4-6. Guest joins by the same RoomId.
      const client2JoinedState = client2
        .waitFor<GameStateNotify>(PacketNames.gameStateNotify)
        .where((message) =>
          message.payload.state.roomId === game.roomId &&
          message.payload.state.oActorId === client2Auth.player.actorId)
        .submit(signal);
      const [client2Join, client2State, client1Running] = await Promise.all([
        client2.request(joinGameReq(game.roomId)).submit<JoinGameRes>(signal),
        client2JoinedState,
        client1SawClient2Join,
        client2.expectNone<PlayerJoinedNotify>(PacketNames.playerJoinedNotify).within(250).run(signal)
      ]);
      connector.zlinkStreamAssert.ensure(stateOf(client2Join).roomId === game.roomId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client2State.payload.state.status === GameStatus.InProgress, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client2State.payload.state.oActorId === client2Auth.player.actorId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client2State.payload.state.xActorId === client1Auth.player.actorId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client2State.payload.state.nextTurn === GameMarks.x, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(client1Running.payload.state.nextTurn === GameMarks.x, 'Sample scenario assertion failed.');
      // 7. Each move response is matched with the opponent notify.
      const client2SawMove1 = waitState(client2, 0, signal);
      const client1Move1 = await client1.request(placeMarkStreamReq(0)).submit<PlaceMarkRes>(signal);
      requireSameState(stateOf(client1Move1), (await client2SawMove1).payload.state);
      connector.zlinkStreamAssert.ensure(stateOf(client1Move1).board === 'X........', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(stateOf(client1Move1).nextTurn === GameMarks.o, 'Sample scenario assertion failed.');
      requireLastMove(stateOf(client1Move1), client1Auth.player.actorId, 0);

      const client1SawMove2 = waitState(client1, 3, signal);
      const client2Move1 = await client2.request(placeMarkStreamReq(3)).submit<PlaceMarkRes>(signal);
      requireSameState(stateOf(client2Move1), (await client1SawMove2).payload.state);
      connector.zlinkStreamAssert.ensure(stateOf(client2Move1).board === 'X..O.....', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(stateOf(client2Move1).nextTurn === GameMarks.x, 'Sample scenario assertion failed.');
      requireLastMove(stateOf(client2Move1), client2Auth.player.actorId, 3);

      const client2SawMove3 = waitState(client2, 1, signal);
      const client1Move2 = await client1.request(placeMarkStreamReq(1)).submit<PlaceMarkRes>(signal);
      requireSameState(stateOf(client1Move2), (await client2SawMove3).payload.state);
      connector.zlinkStreamAssert.ensure(stateOf(client1Move2).board === 'XX.O.....', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(stateOf(client1Move2).nextTurn === GameMarks.o, 'Sample scenario assertion failed.');
      requireLastMove(stateOf(client1Move2), client1Auth.player.actorId, 1);

      const client1SawMove4 = waitState(client1, 4, signal);
      const client2Move2 = await client2.request(placeMarkStreamReq(4)).submit<PlaceMarkRes>(signal);
      requireSameState(stateOf(client2Move2), (await client1SawMove4).payload.state);
      connector.zlinkStreamAssert.ensure(stateOf(client2Move2).board === 'XX.OO....', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(stateOf(client2Move2).nextTurn === GameMarks.x, 'Sample scenario assertion failed.');
      requireLastMove(stateOf(client2Move2), client2Auth.player.actorId, 4);

      const client2SawFinalMove = client2
        .waitFor<GameStateNotify>(PacketNames.gameStateNotify)
        .where((message) =>
          message.payload.state.roomId === game.roomId &&
          message.payload.state.status === GameStatus.Won &&
          message.payload.state.winner === client1Auth.player.actorId)
        .submit(signal);
      const observerSawMilestone = observer
        .waitFor<WinMilestoneNotify>(PacketNames.winMilestoneNotify)
        .where((message) =>
          message.payload.roomId === game.roomId &&
          message.payload.actorId === client1Auth.player.actorId)
        .submit(signal);
      // 8. The final host move wins and publishes the observer milestone.
      const client1FinalMove = await client1.request(placeMarkStreamReq(2)).submit<PlaceMarkRes>(signal);
      requireSameState(stateOf(client1FinalMove), (await client2SawFinalMove).payload.state);
      connector.zlinkStreamAssert.ensure(stateOf(client1FinalMove).board === 'XXXOO....', 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(stateOf(client1FinalMove).status === GameStatus.Won, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(stateOf(client1FinalMove).winner === client1Auth.player.actorId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(stateOf(client1FinalMove).nextTurn === '', 'Sample scenario assertion failed.');
      requireLastMove(stateOf(client1FinalMove), client1Auth.player.actorId, 2);

      const milestone = await observerSawMilestone;
      connector.zlinkStreamAssert.ensure(milestone.payload.roomId === game.roomId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(milestone.payload.actorId === client1Auth.player.actorId, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(milestone.payload.displayName === client1Auth.player.displayName, 'Sample scenario assertion failed.');
      connector.zlinkStreamAssert.ensure(milestone.payload.wins === 100, 'Sample scenario assertion failed.');
      console.log(
        `observer-win-milestone=verified actor=${milestone.payload.actorId} ` +
        `wins=${milestone.payload.wins}`
      );

      await Promise.all([
        client1.send(new LeaveGameMsg(game.roomId)).packetName(PacketNames.leaveGameMsg).submit(),
        client2.send(new LeaveGameMsg(game.roomId)).packetName(PacketNames.leaveGameMsg).submit()
      ]);
      assertInboundObserved(observedClients, 'host');
      assertInboundObserved(observedClients, 'guest');
      assertInboundObserved(observedClients, 'observer');
    } finally {
      await Promise.allSettled([client1.close(), client2.close(), observer.close()]);
    }
  }
}

function waitState(
  client: ZlinkStreamConnector,
  lastMoveCell: number,
  signal?: AbortSignal
): Promise<ZlinkStreamMessage<GameStateNotify>> {
  return client
    .waitFor<GameStateNotify>(PacketNames.gameStateNotify)
    .where((message) => message.payload.state.lastMoveCell === lastMoveCell)
    .submit(signal);
}

function createPlayerClient(endpoint: string, name: string, observedClients: Set<string>): ZlinkStreamConnector {
  const client = connector.zlinkStreamConnectorFactory.create({
    endpoint,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    waitTimeoutMs: 60000,
    heartbeat: { enabled: false }
  });
  client.observeInbound((observation) => {
    if (observation.name.length > 0 && Number.isInteger(observation.kind) && observation.payloadLength >= 0) {
      observedClients.add(name);
    }
    console.log(
      `stream-inbound sample=TicTacToe client=${name} kind=${observation.kind} ` +
      `name=${observation.name} seq=${observation.requestSeq?.toString() ?? '-'} ` +
      `bytes=${observation.payloadLength}`
    );
  });
  return client;
}

function assertInboundObserved(observedClients: ReadonlySet<string>, clientName: string): void {
  connector.zlinkStreamAssert.ensure(observedClients.has(clientName), 'Sample scenario assertion failed.');
}

function stateOf(message: { state: GameState }): GameState {
  return message.state;
}

function requireLastMove(state: GameState, actorId: string, cell: number): void {
  connector.zlinkStreamAssert.ensure(state.lastMoveActorId === actorId, 'Sample scenario assertion failed.');
  connector.zlinkStreamAssert.ensure(state.lastMoveCell === cell, 'Sample scenario assertion failed.');
}

function requireSameState(actual: GameState, expected: GameState): void {
  connector.zlinkStreamAssert.ensure(actual.roomId === expected.roomId, 'Sample scenario assertion failed.');
  connector.zlinkStreamAssert.ensure(actual.board === expected.board, 'Sample scenario assertion failed.');
  connector.zlinkStreamAssert.ensure(actual.status === expected.status, 'Sample scenario assertion failed.');
  connector.zlinkStreamAssert.ensure(actual.winner === expected.winner, 'Sample scenario assertion failed.');
  connector.zlinkStreamAssert.ensure(actual.nextTurn === expected.nextTurn, 'Sample scenario assertion failed.');
  connector.zlinkStreamAssert.ensure(actual.lastMoveActorId === expected.lastMoveActorId, 'Sample scenario assertion failed.');
  connector.zlinkStreamAssert.ensure(actual.lastMoveCell === expected.lastMoveCell, 'Sample scenario assertion failed.');
}

export { TicTacToeClientScenario };
