import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  type ZlinkStreamActor,
  type ZlinkStreamMessage
} from '@zlink-systems/stream-connector';
import {
  Authenticate,
  ChangeNickname,
  PacketNames,
  Ping,
  type Authenticated,
  type NicknameChanged,
  type Pong
} from '../Shared/contracts.js';

async function main(): Promise<void> {
  // --8<-- [start:stream-client]
  // A game client outside the mesh. It references the connector only, never the
  // Framework, and speaks to the port the stream node opened. In Node the
  // connector runs over a WebSocket, so both ends name a ws:// endpoint.
  const connector = zlinkStreamConnectorFactory.create({
    endpoint: 'ws://127.0.0.1:7721',
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    requestTimeoutMs: 5_000,
    waitTimeoutMs: 5_000
  });

  await connector.connect();
  console.log(`connected: ${connector.isConnected}`);

  // A request waits for its reply. Use send for one-way traffic; the server
  // then answers with client.send rather than reply.
  const sentAt = Date.now();
  const pong = await connector
    .request(new Ping(String(sentAt)))
    .timeout(5_000)
    .submit<Pong>();

  console.log(`round trip: ${Date.now() - Number(pong.sentAtUnixMs)}ms`);
  // --8<-- [end:stream-client]

  // --8<-- [start:session-actor-client]
  // --8<-- [start:actor-handle-events]
  const handles = new Map<string, ZlinkStreamActor>();
  const boundNotice = connector.onActorBound((actor) => {
    handles.set(actor.actorId, actor);
    console.log(`actor bound: ${actor.actorId}`);
  });
  const unboundNotice = connector.onActorUnbound((actor) => {
    handles.delete(actor.actorId);
    console.log(`actor unbound: ${actor.actorId}`);
  });
  // --8<-- [end:actor-handle-events]
  // Two characters share this connection. Each authentication binds one Actor.
  const authenticated = await connector
    .request(new Authenticate('p1'))
    .timeout(5_000)
    .submit<Authenticated>();

  console.log(`bound player: ${authenticated.playerId}`);

  // --8<-- [start:single-actor-send]
  // With only p1 bound, a connector-level packet needs no Actor handle.
  let resolveSingle!: (message: ZlinkStreamMessage<NicknameChanged>) => void;
  const singlePush = new Promise<ZlinkStreamMessage<NicknameChanged>>((resolve) => {
    resolveSingle = resolve;
  });
  const receiveSingle = connector.on<NicknameChanged>(PacketNames.nicknameChanged, resolveSingle);
  await connector.send(new ChangeNickname('speedy')).submit();
  const pushedSingle = await singlePush;
  console.log(`pushed: ${pushedSingle.payload.nickname}, actor: ${pushedSingle.actorId}`);
  receiveSingle.dispose();
  // --8<-- [end:single-actor-send]

  // A second Actor on the same connection makes the handle explicit.
  const authenticatedP2 = await connector
    .request(new Authenticate('p2'))
    .timeout(5_000)
    .submit<Authenticated>();

  console.log(`bound player: ${authenticatedP2.playerId}`);

  // --8<-- [start:actor-handle-send]
  const player1 = handles.get(authenticated.playerId);
  if (!player1) throw new Error('Player Actor p1 was not bound.');
  const player2 = handles.get(authenticatedP2.playerId);
  if (!player2) throw new Error('Player Actor p2 was not bound.');
  console.log(`actor handle: ${player1.actorId}`);
  console.log(`actor handle: ${player2.actorId}`);
  // --8<-- [end:actor-handle-send]

  // --8<-- [start:actor-handle-per-handle-receive]
  // Each callback receives only the push for its handle's Actor.
  let resolveChanged1!: (message: ZlinkStreamMessage<NicknameChanged>) => void;
  let resolveChanged2!: (message: ZlinkStreamMessage<NicknameChanged>) => void;
  const changed1 = new Promise<ZlinkStreamMessage<NicknameChanged>>((resolve) => {
    resolveChanged1 = resolve;
  });
  const changed2 = new Promise<ZlinkStreamMessage<NicknameChanged>>((resolve) => {
    resolveChanged2 = resolve;
  });
  const receive1 = player1.on<NicknameChanged>(PacketNames.nicknameChanged, resolveChanged1);
  const receive2 = player2.on<NicknameChanged>(PacketNames.nicknameChanged, resolveChanged2);
  // --8<-- [end:actor-handle-per-handle-receive]

  // --8<-- [start:actor-id-receive]
  // Connector-level callbacks see every push and can tell the Actors apart.
  let resolveActorIds!: () => void;
  const actorIdsReceived = new Promise<void>((resolve) => {
    resolveActorIds = resolve;
  });
  let actorIdsSeen = 0;
  const receiveActorId = connector.on<NicknameChanged>(PacketNames.nicknameChanged, (message) => {
    console.log(`received actor id: ${message.actorId}`);
    if (++actorIdsSeen === 2) resolveActorIds();
  });
  // --8<-- [end:actor-id-receive]

  // Each handle sends to its own player over the same connection.
  // --8<-- [start:actor-handle-send-call]
  await player1.send(new ChangeNickname('speedy-p1')).submit();
  await player2.send(new ChangeNickname('speedy-p2')).submit();
  // --8<-- [end:actor-handle-send-call]

  // --8<-- [start:actor-handle-receive]
  const pushed1 = await changed1;
  const pushed2 = await changed2;
  await actorIdsReceived;
  console.log(`pushed: ${pushed1.payload.nickname}, actor: ${pushed1.actorId}`);
  console.log(`pushed: ${pushed2.payload.nickname}, actor: ${pushed2.actorId}`);
  // --8<-- [end:actor-handle-receive]

  // --8<-- [end:session-actor-client]

  receive1.dispose();
  receive2.dispose();
  receiveActorId.dispose();
  await connector.close();
  boundNotice.dispose();
  unboundNotice.dispose();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
