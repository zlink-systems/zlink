import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
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
  const boundNotice = connector.onActorBound((actor) => {
    console.log(`actor bound: ${actor.actorId}`);
  });
  const unboundNotice = connector.onActorUnbound((actor) => {
    console.log(`actor unbound: ${actor.actorId}`);
  });
  // --8<-- [end:actor-handle-events]
  // Binds this connection to a player. Until then the server has no player to
  // forward packets to.
  const authenticated = await connector
    .request(new Authenticate('p1'))
    .timeout(5_000)
    .submit<Authenticated>();

  console.log(`bound player: ${authenticated.playerId}`);

  // --8<-- [start:actor-handle-send]
  const player = connector.actor(authenticated.playerId);
  if (!player) throw new Error('Player Actor was not bound');
  console.log(`actor handle: ${player.actorId}`);
  // --8<-- [end:actor-handle-send]

  // Arrange to receive the push before sending, so a fast server cannot answer
  // before the client is listening.
  const changed = connector
    .waitFor<NicknameChanged>(PacketNames.nicknameChanged)
    .timeout(5_000)
    .submit();

  // The handle addresses this player directly. Its handler pushes the result
  // back over the same connection.
  // --8<-- [start:actor-handle-send-call]
  await player.send(new ChangeNickname('speedy')).submit();
  // --8<-- [end:actor-handle-send-call]

  // --8<-- [start:actor-handle-receive]
  const pushed = await changed;
  console.log(`pushed: ${pushed.payload.nickname}, actor: ${pushed.actorId}`);
  // --8<-- [end:actor-handle-receive]
  // --8<-- [end:session-actor-client]

  await connector.close();
  boundNotice.dispose();
  unboundNotice.dispose();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
