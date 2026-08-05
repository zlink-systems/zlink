import {
  BingoRewardItems,
  BingoRoomStatus,
  BingoSamplePlayers,
  PacketNames,
  deterministicCard
} from '../Shared/Contracts/messages';
import {
  AuthenticateReq,
  MatchBingoReq,
  ObserveBingoEventsReq,
  StopObservingBingoEventsReq,
  SubmitBingoCardReq
} from '../Shared/Contracts/bingo-messages.generated';
import type {
  AuthenticateRes,
  BingoRewardAnnouncedNotify,
  MatchBingoRes,
  NumberDrawnNotify,
  ObserveBingoEventsRes,
  PlayerJoinedNotify,
  StateEnvelope,
  StopObservingBingoEventsRes,
  SubmitBingoCardRes
} from '../Shared/Contracts/messages';
import { zlinkStreamAssert } from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector, ZlinkStreamMessage } from '@zlink-systems/stream-connector';

type BingoRoomState = {
  roomId: string;
  status: BingoRoomStatus;
  hostActorId: string | null;
  drawSeq: number;
  winners: string[];
  drawnNumbers: number[];
  players: Array<{
    actorId: string;
    card: number[];
    marks: boolean[];
    wins: number;
    losses: number;
  }>;
};

class BingoClientScenario {
  async run(
    client1: ZlinkStreamConnector,
    client2: ZlinkStreamConnector,
    observer: ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    // 1. Clients connect only to Session streams, authenticate, and verify actor ids.
    await client1.connect(signal);
    await client2.connect(signal);
    await observer.connect(signal);

    const client1Auth = await client1.request(new AuthenticateReq({ accessToken: BingoSamplePlayers.player1 }))
      .packetName(PacketNames.authenticateReq).submit<AuthenticateRes>(signal);
    const client2Auth = await client2.request(new AuthenticateReq({ accessToken: BingoSamplePlayers.player2 }))
      .packetName(PacketNames.authenticateReq).submit<AuthenticateRes>(signal);
    const observerAuth = await observer.request(new AuthenticateReq({ accessToken: BingoSamplePlayers.observer }))
      .packetName(PacketNames.authenticateReq).submit<AuthenticateRes>(signal);

    zlinkStreamAssert.ensure(client1Auth.actorId === BingoSamplePlayers.player1, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(client2Auth.actorId === BingoSamplePlayers.player2, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(observerAuth.actorId === BingoSamplePlayers.observer, 'Sample scenario assertion failed.');

    // 2. player-1 matches first, gets a waiting room, and receives no self-join notify.
    const [client1MatchRes] = await Promise.all([
      client1.request(new MatchBingoReq({ mode: 'two-player' }))
        .packetName(PacketNames.matchBingoReq).submit<MatchBingoRes>(signal),
      client1.expectNone<PlayerJoinedNotify>(PacketNames.playerJoinedNotify).within(250).run(signal)
    ]);

    zlinkStreamAssert.ensure(client1MatchRes.roomId.length > 0, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1MatchRes).roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1MatchRes).status === BingoRoomStatus.WaitingForPlayers, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1MatchRes).hostActorId === client1Auth.actorId, 'Sample scenario assertion failed.');

    // 3. The third client observes rewards through a local BingoRoom on SessionB's Play node.
    const observed = await observer
      .request(new ObserveBingoEventsReq({ roomId: client1MatchRes.roomId }))
      .packetName(PacketNames.observeBingoEventsReq)
      .submit<ObserveBingoEventsRes>(signal);
    zlinkStreamAssert.ensure(observed.subscribed, 'Sample scenario assertion failed.');
    const observerRewardTask = observer
      .waitFor<BingoRewardAnnouncedNotify>(PacketNames.rewardAnnouncedNotify)
      .where((message) => message.payload.roomId === client1MatchRes.roomId)
      .submit(signal);

    // 4-6. player-2 joins the same room; player-1 observes join and both clients observe start.
    const client1SawClient2Join = client1
      .waitFor<PlayerJoinedNotify>(PacketNames.playerJoinedNotify)
      .where((message) => message.payload.actorId === client2Auth.actorId)
      .submit(signal);
    const client1StartedTask = client1.waitFor<StateEnvelope>(PacketNames.gameStartedNotify).submit(signal);
    const client2StartedTask = client2.waitFor<StateEnvelope>(PacketNames.gameStartedNotify).submit(signal);
    const [client2MatchRes] = await Promise.all([
      client2.request(new MatchBingoReq({ mode: 'two-player' }))
        .packetName(PacketNames.matchBingoReq).submit<MatchBingoRes>(signal),
      client2.expectNone<PlayerJoinedNotify>(PacketNames.playerJoinedNotify).within(250).run(signal)
    ]);

    zlinkStreamAssert.ensure(client2MatchRes.roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(client2MatchRes.roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client2MatchRes).roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    // Node's exact Actor contract registers this join with defer(), so the
    // immediate MatchBingoRes is a pre-join projection. The authoritative
    // Running state is checked on both start notifications below.
    zlinkStreamAssert.ensure(
      stateOf(client2MatchRes).status === BingoRoomStatus.WaitingForPlayers,
      'Sample scenario assertion failed.'
    );

    const [client1Joined, client1Started, client2Started] = await Promise.all([
      client1SawClient2Join,
      client1StartedTask,
      client2StartedTask
    ]);
    zlinkStreamAssert.ensure(client1Joined.payload.roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(client1Joined.payload.actorId === client2Auth.actorId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Joined.payload).status === BingoRoomStatus.Running, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Joined.payload).roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Started.payload).status === BingoRoomStatus.Running, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Started.payload).roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client2Started.payload).status === BingoRoomStatus.Running, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client2Started.payload).roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Started.payload).players.every((player) => player.wins === 0), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Started.payload).players.every((player) => player.losses === 0), 'Sample scenario assertion failed.');

    // 7. Both clients submit deterministic cards and responses show both 3 x 3 cards.
    const client2Card = await client2
      .request(new SubmitBingoCardReq({
        roomId: client2MatchRes.roomId,
        card: deterministicCard(client2Auth.actorId)
      }))
      .packetName(PacketNames.submitBingoCardReq)
      .submit<SubmitBingoCardRes>(signal);

    zlinkStreamAssert.ensure(stateOf(client2Card).status === BingoRoomStatus.Running, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client2Card).players.find((player) => player.actorId === client2Auth.actorId)?.card.length === 9, 'Sample scenario assertion failed.');

    const drawWaitController = new AbortController();
    const abortDrawWaits = () => drawWaitController.abort();
    signal?.addEventListener('abort', abortDrawWaits, { once: true });
    const drawTasks = Array.from({ length: 15 }, (_, index) => {
      const drawSeq = index + 1;
      return {
        drawSeq,
        client1: waitForDraw(client1, drawSeq, drawWaitController.signal),
        client2: waitForDraw(client2, drawSeq, drawWaitController.signal)
      };
    });
    const client1EndedTask = client1.waitFor<StateEnvelope>(PacketNames.gameEndedNotify).submit(signal);
    const client2EndedTask = client2.waitFor<StateEnvelope>(PacketNames.gameEndedNotify).submit(signal);

    const client1Card = await client1
      .request(new SubmitBingoCardReq({
        roomId: client1MatchRes.roomId,
        card: deterministicCard(client1Auth.actorId)
      }))
      .packetName(PacketNames.submitBingoCardReq)
      .submit<SubmitBingoCardRes>(signal);

    zlinkStreamAssert.ensure(stateOf(client1Card).status === BingoRoomStatus.Running, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Card).players.every((player) => player.card.length === 9), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client1Card).players.length === 2, 'Sample scenario assertion failed.');

    // 8. Number drawing is server-driven; clients only wait for draw notifications.
    const drawnNumbers: NumberDrawnNotify[] = [];
    try {
      for (const drawTask of drawTasks) {
        const [client1Draw, client2Draw] = await Promise.all([
          drawTask.client1,
          drawTask.client2
        ]);
        requireSameDraw(client1Draw.payload, client2Draw.payload, drawTask.drawSeq);
        drawnNumbers.push(client1Draw.payload);
        if (stateOf(client1Draw.payload).status === BingoRoomStatus.Finished) {
          break;
        }
      }
    } finally {
      drawWaitController.abort();
      signal?.removeEventListener('abort', abortDrawWaits);
      await Promise.allSettled(drawTasks.flatMap((drawTask) => [drawTask.client1, drawTask.client2]));
    }

    zlinkStreamAssert.ensure(drawnNumbers.length > 0, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(drawnNumbers[drawnNumbers.length - 1]).status === BingoRoomStatus.Finished, 'Sample scenario assertion failed.');

    // 9. Both clients receive the final finished state when the server detects bingo.
    const [client1Ended, client2Ended] = await Promise.all([client1EndedTask, client2EndedTask]);
    zlinkStreamAssert.ensure(stateOf(client1Ended.payload).status === BingoRoomStatus.Finished, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client2Ended.payload).status === BingoRoomStatus.Finished, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client2Ended.payload).drawnNumbers.join(',') === stateOf(client1Ended.payload).drawnNumbers.join(','), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(stateOf(client2Ended.payload).winners.join(',') === stateOf(client1Ended.payload).winners.join(','), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(
      stateOf(client2Ended.payload).players.map((player) => player.actorId).join(',') ===
      stateOf(client1Ended.payload).players.map((player) => player.actorId).join(','),
      'Sample scenario assertion failed.'
    );

    const started = stateOf(client1Started.payload);
    const ended = stateOf(client1Ended.payload);
    zlinkStreamAssert.ensure(started.status === BingoRoomStatus.Running, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(ended.status === BingoRoomStatus.Finished, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(drawnNumbers.map((draw) => draw.number).join(',') === ended.drawnNumbers.join(','), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(ended.winners.join(',') === client1Auth.actorId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(ended.players.every((player) => player.card.length === 9), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(ended.players.every((player) => player.marks[4]), 'Sample scenario assertion failed.');

    // 10-11. Observer receives the reward through Spot pub/sub and then stops observing.
    const reward = await observerRewardTask;
    zlinkStreamAssert.ensure(reward.payload.roomId === client1MatchRes.roomId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(reward.payload.actorId === client1Auth.actorId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(reward.payload.drawSeq === ended.drawSeq, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(reward.payload.itemId === BingoRewardItems.goldenDauberId, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(reward.payload.itemName === BingoRewardItems.goldenDauberName, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(reward.payload.rarity === BingoRewardItems.legendaryRarity, 'Sample scenario assertion failed.');

    const stopped = await observer
      .request(new StopObservingBingoEventsReq({ roomId: client1MatchRes.roomId }))
      .packetName(PacketNames.stopObservingBingoEventsReq)
      .submit<StopObservingBingoEventsRes>(signal);
    zlinkStreamAssert.ensure(stopped.stopped, 'Sample scenario assertion failed.');
  }
}

function stateOf(message: { state: unknown } | StateEnvelope | NumberDrawnNotify | PlayerJoinedNotify): BingoRoomState {
  return message.state as BingoRoomState;
}

function requireSameDraw(client1Draw: NumberDrawnNotify, client2Draw: NumberDrawnNotify, expectedSeq: number): void {
  zlinkStreamAssert.ensure(client1Draw.drawSeq === expectedSeq, 'Sample scenario assertion failed.');
  zlinkStreamAssert.ensure(client2Draw.drawSeq === expectedSeq, 'Sample scenario assertion failed.');
  zlinkStreamAssert.ensure(client2Draw.drawSeq === client1Draw.drawSeq, 'Sample scenario assertion failed.');
  zlinkStreamAssert.ensure(client2Draw.number === client1Draw.number, 'Sample scenario assertion failed.');
  zlinkStreamAssert.ensure(stateOf(client2Draw).drawnNumbers.join(',') === stateOf(client1Draw).drawnNumbers.join(','), 'Sample scenario assertion failed.');
}

function waitForDraw(
  client: ZlinkStreamConnector,
  drawSeq: number,
  signal?: AbortSignal
): Promise<ZlinkStreamMessage<NumberDrawnNotify>> {
  return client
    .waitFor<NumberDrawnNotify>(PacketNames.numberDrawnNotify)
    .where((message) => message.payload.drawSeq === drawSeq)
    .submit(signal);
}

export { BingoClientScenario };
