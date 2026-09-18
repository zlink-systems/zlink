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
  // Binds this connection to a player. Until then the server has no player to
  // forward packets to.
  const authenticated = await connector
    .request(new Authenticate('p1'))
    .timeout(5_000)
    .submit<Authenticated>();

  console.log(`bound player: ${authenticated.playerId}`);

  // Arrange to receive the push before sending, so a fast server cannot answer
  // before the client is listening.
  const changed = connector
    .waitFor<NicknameChanged>(PacketNames.nicknameChanged)
    .timeout(5_000)
    .submit();

  // No session handler matches this packet, so the session relays it to the
  // bound player, whose handler pushes the result back over this same
  // connection.
  await connector.send(new ChangeNickname('speedy')).submit();

  console.log(`pushed: ${(await changed).payload.nickname}`);
  // --8<-- [end:session-actor-client]

  await connector.close();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
