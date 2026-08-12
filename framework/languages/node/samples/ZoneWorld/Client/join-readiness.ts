import { zlinkStreamAssert } from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { JoinWorldReq, PacketNames } from '../Shared/contracts';
import type { JoinWorldRes, ZoneStateNotify } from '../Shared/contracts';

async function joinAndWaitForOwnedState(
  client: ZlinkStreamConnector,
  playerId: string
): Promise<JoinWorldRes> {
  // Client push is not replayed. Arm before the request so the first state
  // emitted after the deferred join commit cannot pass the consumer.
  const ownedStateTask = client
    .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => message.payload.players.some((player) => player.playerId === playerId))
    .timeout(10_000)
    .submit();
  const joinedTask = client
    .request(new JoinWorldReq(playerId))
    .packetName(PacketNames.joinWorldReq)
    .submit<JoinWorldRes>();
  const [joined, ownedState] = await Promise.all([joinedTask, ownedStateTask]);
  zlinkStreamAssert.ensure(joined.error === null, `Player '${playerId}' join was rejected.`);
  zlinkStreamAssert.ensure(
    ownedState.payload.zoneId === joined.zoneId
      && ownedState.payload.players.some((player) =>
        player.playerId === joined.playerId && player.x === joined.x && player.y === joined.y),
    `Player '${playerId}' initial owned-zone state did not match JoinWorldRes.`
  );
  return joined;
}

export { joinAndWaitForOwnedState };
