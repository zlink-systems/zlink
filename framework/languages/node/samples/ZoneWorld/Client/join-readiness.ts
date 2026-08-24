import { zlinkStreamAssert } from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { JoinWorldReq, PacketNames } from '../Shared/contracts';
import type { JoinWorldRes, ZoneStateNotify } from '../Shared/contracts';

async function joinAndWaitForOwnedState(
  client: ZlinkStreamConnector,
  playerId: string,
  expectedZoneId?: string
): Promise<JoinWorldRes> {
  // Client pushes are not replayed. Arm both completion-owned JoinWorldRes
  // and the first owned state before submitting JoinWorldReq.
  const joinedTask = client
    .waitFor<JoinWorldRes>(PacketNames.joinWorldRes)
    .where((message) => message.payload.playerId === playerId)
    .timeout(10_000)
    .submit();
  const ownedStateTask = client
    .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => message.payload.players.some((player) =>
      player.playerId === playerId
        && player.zoneId === message.payload.zoneId
        && (expectedZoneId === undefined || message.payload.zoneId === expectedZoneId)))
    .timeout(10_000)
    .submit();
  await client.send(new JoinWorldReq(playerId)).packetName(PacketNames.joinWorldReq).submit();
  const [joinedMessage, ownedState] = await Promise.all([joinedTask, ownedStateTask]);
  const joined = joinedMessage.payload;
  zlinkStreamAssert.ensure(joined.error === null, `Player '${playerId}' join was rejected.`);
  zlinkStreamAssert.ensure(
    ownedState.payload.zoneId === joined.zoneId
      && ownedState.payload.players.some((player) =>
        player.playerId === joined.playerId
          && player.zoneId === joined.zoneId
          && player.x === joined.x
          && player.y === joined.y),
    `Player '${playerId}' initial owned-zone state did not match JoinWorldRes.`
  );
  return joined;
}

export { joinAndWaitForOwnedState };
