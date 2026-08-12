import * as fs from 'node:fs';
import {
  ZlinkStreamDispatchMode,
  zlinkStreamAssert,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import {
  ActorLocationProbeReq,
  AnnounceWorldReq,
  MessageFollowProbeReq,
  MessageFollowProbeMsg,
  MoveMsg,
  PacketNames,
  WatchNodesReq
} from '../Shared/contracts';
import { NodeIds, ZoneIds, ZoneWorldSpec } from '../Shared/spec';
import { readConfigPath, validateConfiguration } from '../Server/Configuration/configuration';
import type {
  ActorLocationProbeRes,
  AnnounceWorldRes,
  MessageFollowProbeRes,
  MoveRejectedNotify,
  NodeStatusNotify,
  WatchNodesRes,
  WorldAnnounceNotify,
  ZoneChangedNotify,
  ZoneStateNotify
} from '../Shared/contracts';
import { joinAndWaitForOwnedState } from './join-readiness';

async function main(): Promise<void> {
  const path = readConfigPath(process.argv.slice(2));
  const config = validateConfiguration(JSON.parse(fs.readFileSync(path, 'utf8')) as unknown, 'client');
  if (config.client === undefined) throw new Error('Client configuration is required.');
  const gateway = createConnector(config.client.gatewayEndpoint);
  const second = createConnector(config.client.gatewayEndpoint);
  const westObserver = createConnector(config.client.gatewayEndpoint);
  const ops = createConnector(config.client.opsEndpoint);
  try {
    await gateway.connect();
    const joined = await joinAndWaitForOwnedState(gateway, 'player-a1');
    zlinkStreamAssert.ensure(joined.playerId === 'player-a1', 'ZW-A1 player id mismatch.');
    zlinkStreamAssert.ensure(joined.zoneId === ZoneIds.northWest, 'ZW-A1 spawn zone mismatch.');
    zlinkStreamAssert.ensure(joined.nodeId === NodeIds.west, 'ZW-A1 spawn node mismatch.');
    zlinkStreamAssert.ensure(
      joined.x === ZoneWorldSpec.spawnX && joined.y === ZoneWorldSpec.spawnY,
      'ZW-A1 spawn coordinate mismatch.'
    );
    zlinkStreamAssert.ensure(joined.error === null, 'ZW-A1 join was rejected.');
    console.log('scenario ZW-A1 passed');

    const rejectedTask = gateway
      .waitFor<MoveRejectedNotify>(PacketNames.moveRejectedNotify)
      .where((message) => message.payload.reason === 'OutOfRange')
      .submit();
    await gateway.send(new MoveMsg(-40, joined.y)).packetName(PacketNames.moveMsg).submit();
    const rejected = await rejectedTask;
    zlinkStreamAssert.ensure(
      rejected.payload.x === joined.x && rejected.payload.y === joined.y,
      'ZW-A2 rejection changed the player coordinate.'
    );
    console.log('scenario ZW-A2 passed');

    const movedTask = gateway
      .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) =>
        player.playerId === joined.playerId && player.x === joined.x + 4 && player.y === joined.y))
      .submit();
    await gateway.send(new MoveMsg(joined.x + 4, joined.y)).packetName(PacketNames.moveMsg).submit();
    await movedTask;
    console.log('scenario ZW-A5 passed');

    await second.connect();
    const joinedSecond = await joinAndWaitForOwnedState(second, 'player-a3');
    zlinkStreamAssert.ensure(joinedSecond.error === null, 'ZW-A3 second player join was rejected.');
    const expectedPlayers = ['player-a1', 'player-a3'];
    const [firstView, secondView] = await Promise.all([
      waitForPlayers(gateway, expectedPlayers),
      waitForPlayers(second, expectedPlayers)
    ]);
    zlinkStreamAssert.ensure(
      firstView.payload.players.map((player) => player.playerId).filter((id) => expectedPlayers.includes(id)).join(',')
        === expectedPlayers.join(','),
      'ZW-A3 first client player order mismatch.'
    );
    zlinkStreamAssert.ensure(
      secondView.payload.players.map((player) => player.playerId).filter((id) => expectedPlayers.includes(id)).join(',')
        === expectedPlayers.join(','),
      'ZW-A3 second client player order mismatch.'
    );
    console.log('scenario ZW-A3 passed');

    let position: { x: number; y: number } = { x: joined.x + 4, y: joined.y };
    position = await walkTo(gateway, joined.playerId, position, 49, 49);
    const diagonalRejected = gateway
      .waitFor<MoveRejectedNotify>(PacketNames.moveRejectedNotify)
      .where((message) => message.payload.reason === 'DiagonalCrossing')
      .submit();
    await gateway.send(new MoveMsg(50, 50)).packetName(PacketNames.moveMsg).submit();
    const diagonal = await diagonalRejected;
    zlinkStreamAssert.ensure(
      diagonal.payload.x === position.x && diagonal.payload.y === position.y,
      'ZW-A4 diagonal rejection changed the player coordinate.'
    );
    console.log('scenario ZW-A4 passed');

    let secondPosition: { x: number; y: number } = { x: joinedSecond.x, y: joinedSecond.y };
    secondPosition = await walkTo(second, joinedSecond.playerId, secondPosition, 25, 48);
    const internalChanged = second
      .waitFor<ZoneChangedNotify>(PacketNames.zoneChangedNotify)
      .where((message) => message.payload.zoneId === ZoneIds.southWest)
      .submit();
    await second.send(new MoveMsg(25, 52)).packetName(PacketNames.moveMsg).submit();
    const internal = await internalChanged;
    zlinkStreamAssert.ensure(!internal.payload.transferred, 'ZW-B3 node-local zone move transferred the actor.');
    zlinkStreamAssert.ensure(internal.payload.nodeId === NodeIds.west, 'ZW-B3 node id changed.');
    console.log('scenario ZW-B3 passed');

    // Arm the adjacent-zone observer before the producer crosses the node
    // boundary. Publish is a one-way event and is not replayed to a consumer
    // that becomes ready after the event has already been delivered.
    await westObserver.connect();
    const westJoined = await joinAndWaitForOwnedState(westObserver, 'player-b1-west');
    // JoinWorldRes describes the accepted target. The first owned-zone state
    // push proves that join commit and the bound-session route are ready before
    // the observer starts submitting movement messages.
    await walkTo(
      westObserver,
      westJoined.playerId,
      { x: westJoined.x, y: westJoined.y },
      45,
      25
    );
    const adjacentViewTask = westObserver
      .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) =>
        player.playerId === joined.playerId && player.zoneId === ZoneIds.northEast))
      .timeout(10_000)
      .submit();

    console.log('scenario ZW-B2 preparing cross-node move');
    position = await walkTo(gateway, joined.playerId, position, 49, 25);
    console.log('scenario ZW-B2 source position ready');
    // ZW-B5/ZW-B6 arm before the relocation: capture the actor's identity on
    // the source owner and prime the Gateway's Actor route so the probe sent
    // right after the move enters the previous owner's route.
    const beforeRelocation = await probeActor(gateway, joined.playerId);
    zlinkStreamAssert.ensure(beforeRelocation.error === null, 'ZW-B5 pre-relocation probe failed.');
    zlinkStreamAssert.ensure(
      beforeRelocation.actorId === joined.playerId,
      'ZW-B5 pre-relocation probe resolved a different actor.'
    );
    const primed = await probeMessageFollow(gateway, joined.playerId, 'zw-b6-prime', 'route-prime');
    zlinkStreamAssert.ensure(
      primed.error === null && primed.probeId === 'zw-b6-prime' && primed.payload === 'route-prime',
      'ZW-B6 route prime changed the payload or reply correlation.'
    );
    const transferredTask = gateway
      .waitFor<ZoneChangedNotify>(PacketNames.zoneChangedNotify)
      .where((message) => message.payload.zoneId === ZoneIds.northEast)
      .submit();
    await gateway.send(new MoveMsg(52, 25)).packetName(PacketNames.moveMsg).submit();
    console.log('scenario ZW-B2 move submitted');
    const transferred = await transferredTask;
    console.log('scenario ZW-B2 change observed');
    zlinkStreamAssert.ensure(transferred.payload.transferred, 'ZW-B2 cross-node move did not transfer the actor.');
    zlinkStreamAssert.ensure(transferred.payload.nodeId === NodeIds.east, 'ZW-B2 target node mismatch.');
    await moveAndWait(gateway, joined.playerId, 55, 25);
    const adjacentView = await adjacentViewTask;
    console.log('scenario ZW-B2 target position ready');
    console.log('scenario ZW-B2 passed');

    // ZW-B5: the cross-node move relocated the actor without changing its
    // identity. The operational probe resolves the same ActorId with the same
    // ObjectGeneration on a different owner node.
    const afterRelocation = await probeActor(gateway, joined.playerId);
    zlinkStreamAssert.ensure(afterRelocation.error === null, 'ZW-B5 post-relocation probe failed.');
    zlinkStreamAssert.ensure(afterRelocation.actorId === joined.playerId, 'ZW-B5 relocation changed the ActorId.');
    zlinkStreamAssert.ensure(
      afterRelocation.objectGeneration === beforeRelocation.objectGeneration,
      'ZW-B5 relocation changed the ObjectGeneration.'
    );
    zlinkStreamAssert.ensure(
      afterRelocation.nodeRid !== beforeRelocation.nodeRid,
      'ZW-B5 relocation did not change the current owner node.'
    );
    console.log('scenario ZW-B5 passed');

    // ZW-B6: messages submitted right after the relocation may enter the
    // previous owner via the primed route; Message Follow must deliver them to
    // the committed target exactly once and answer with the payload intact.
    await gateway
      .send(new MessageFollowProbeMsg(joined.playerId, 'zw-b6-one-way', 'one-way-payload'))
      .packetName(PacketNames.messageFollowProbeMsg)
      .submit();
    const followed = await probeMessageFollow(gateway, joined.playerId, 'zw-b6-request', 'request-payload');
    zlinkStreamAssert.ensure(
      followed.error === null && followed.probeId === 'zw-b6-request' && followed.payload === 'request-payload',
      'ZW-B6 the followed request lost its payload or reply correlation.'
    );
    // The documented terminal bound: with no route at all there is nothing to
    // follow, so the probe ends with a terminal error instead of retrying.
    const unroutable = await probeMessageFollow(gateway, 'player-b6-missing', 'zw-b6-missing', 'missing-payload');
    zlinkStreamAssert.ensure(
      unroutable.error !== null,
      'ZW-B6 a probe without an actor route must end with a terminal error.'
    );
    console.log('scenario ZW-B6 passed');

    zlinkStreamAssert.ensure(
      adjacentView.payload.players.some((player) =>
        player.playerId === joined.playerId && player.zoneId === ZoneIds.northEast),
      'ZW-B1 adjacent zone player was not visible.'
    );
    const settledDiagonal = await second
      .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => !message.payload.players.some((player) => player.playerId === joined.playerId))
      .timeout(10_000)
      .submit();
    let lastTick = settledDiagonal.payload.tick;
    for (let index = 0; index < 3; index += 1) {
      const diagonalView = await second
        .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
        .where((message) => message.payload.tick > lastTick)
        .submit();
      lastTick = diagonalView.payload.tick;
      zlinkStreamAssert.ensure(
        !diagonalView.payload.players.some((player) => player.playerId === joined.playerId),
        'ZW-B1 diagonal zone received a border snapshot.'
      );
    }
    console.log('scenario ZW-B1 passed');

    // ZW-B7: repeated relocation round trip (A -> B -> A). The same player
    // crosses the same boundary back to the original node. Receiving the
    // ZoneChangedNotify and the settling ZoneStateNotify on the same
    // still-bound gateway connection is the binding-continuity evidence.
    position = await walkTo(gateway, joined.playerId, { x: 55, y: 25 }, 52, 25);
    const returnedTask = gateway
      .waitFor<ZoneChangedNotify>(PacketNames.zoneChangedNotify)
      .where((message) => message.payload.zoneId === ZoneIds.northWest)
      .submit();
    const resettledTask = gateway
      .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.zoneId === ZoneIds.northWest
        && message.payload.players.some((player) =>
          player.playerId === joined.playerId && player.x === 48 && player.y === 25))
      .submit();
    await gateway.send(new MoveMsg(48, 25)).packetName(PacketNames.moveMsg).submit();
    const returned = await returnedTask;
    zlinkStreamAssert.ensure(returned.payload.playerId === joined.playerId, 'ZW-B7 return relocation changed the actor id.');
    zlinkStreamAssert.ensure(returned.payload.transferred, 'ZW-B7 return move did not transfer the actor.');
    zlinkStreamAssert.ensure(returned.payload.nodeId === NodeIds.west, 'ZW-B7 return target node mismatch.');
    await resettledTask;
    position = { x: 48, y: 25 };
    const afterReturn = await probeActor(gateway, joined.playerId);
    zlinkStreamAssert.ensure(afterReturn.error === null, 'ZW-B7 post-return probe failed.');
    zlinkStreamAssert.ensure(afterReturn.actorId === joined.playerId, 'ZW-B7 round trip changed the ActorId.');
    zlinkStreamAssert.ensure(
      afterReturn.objectGeneration === beforeRelocation.objectGeneration,
      'ZW-B7 round trip changed the ObjectGeneration.'
    );
    zlinkStreamAssert.ensure(
      afterReturn.nodeRid === beforeRelocation.nodeRid,
      'ZW-B7 round trip did not return the actor to its original owner node.'
    );
    console.log('scenario ZW-B7 passed');

    await ops.connect();
    const nodes = await ops
      .request(new WatchNodesReq())
      .packetName(PacketNames.watchNodesReq)
      .submit<WatchNodesRes>();
    zlinkStreamAssert.ensure(nodes.nodes.length === 2, 'ZW-C1 node snapshot did not contain both nodes.');
    const westStatusTask = ops
      .waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
      .where((message) => message.payload.nodeId === NodeIds.west)
      .timeout(10_000)
      .submit();
    const eastStatusTask = ops
      .waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
      .where((message) => message.payload.nodeId === NodeIds.east)
      .timeout(10_000)
      .submit();
    const [westStatus, eastStatus] = await Promise.all([westStatusTask, eastStatusTask]);
    zlinkStreamAssert.ensure(
      westStatus.payload.registered && westStatus.payload.connected,
      'ZW-C1 west node was not registered and connected.'
    );
    zlinkStreamAssert.ensure(
      eastStatus.payload.registered && eastStatus.payload.connected,
      'ZW-C1 east node was not registered and connected.'
    );
    console.log('scenario ZW-C1 passed');

    const announcementTask = gateway
      .waitFor<WorldAnnounceNotify>(PacketNames.worldAnnounceNotify)
      .where((message) => message.payload.text === 'world-ready')
      .timeout(10_000)
      .submit();
    const announced = await ops
      .request(new AnnounceWorldReq('world-ready'))
      .packetName(PacketNames.announceWorldReq)
      .submit<AnnounceWorldRes>();
    const announcement = await announcementTask;
    zlinkStreamAssert.ensure(
      announcement.payload.announcementId === announced.announcementId,
      'ZW-D1 announcement id mismatch.'
    );
    console.log('scenario ZW-D1 passed');
  } finally {
    await Promise.allSettled([gateway.close(), second.close(), westObserver.close(), ops.close()]);
  }
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};

function createConnector(endpoint: string): ZlinkStreamConnector {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    waitTimeoutMs: 10_000,
    heartbeat: { enabled: false }
  });
}

async function probeActor(client: ZlinkStreamConnector, actorId: string): Promise<ActorLocationProbeRes> {
  return await client
    .request(new ActorLocationProbeReq(actorId))
    .packetName(PacketNames.actorLocationProbeReq)
    .submit<ActorLocationProbeRes>();
}

async function probeMessageFollow(
  client: ZlinkStreamConnector,
  actorId: string,
  probeId: string,
  payload: string
): Promise<MessageFollowProbeRes> {
  return await client
    .request(new MessageFollowProbeReq(actorId, probeId, payload))
    .packetName(PacketNames.messageFollowProbeReq)
    .submit<MessageFollowProbeRes>();
}

function waitForPlayers(client: ZlinkStreamConnector, playerIds: readonly string[]) {
  return client.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => playerIds.every((id) => message.payload.players.some((player) => player.playerId === id)))
    .submit();
}

async function walkTo(
  client: ZlinkStreamConnector,
  playerId: string,
  from: { x: number; y: number },
  targetX: number,
  targetY: number
): Promise<{ x: number; y: number }> {
  let current = from;
  while (current.x !== targetX || current.y !== targetY) {
    const next = {
      x: stepToward(current.x, targetX),
      y: stepToward(current.y, targetY)
    };
    await moveAndWait(client, playerId, next.x, next.y);
    current = next;
  }
  return current;
}

async function moveAndWait(
  client: ZlinkStreamConnector,
  playerId: string,
  x: number,
  y: number
): Promise<void> {
  const observed = client.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => message.payload.players.some((player) =>
      player.playerId === playerId && player.x === x && player.y === y))
    .submit();
  await client.send(new MoveMsg(x, y)).packetName(PacketNames.moveMsg).submit();
  await observed;
}

function stepToward(value: number, target: number): number {
  if (value === target) return value;
  return value + Math.sign(target - value) * Math.min(5, Math.abs(target - value));
}
