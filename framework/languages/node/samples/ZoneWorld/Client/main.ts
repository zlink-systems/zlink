import * as fs from 'node:fs';
import {
  ZlinkStreamDispatchMode,
  zlinkStreamAssert,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { AnnounceWorldReq, JoinWorldReq, MoveMsg, PacketNames, WatchNodesReq } from '../Shared/contracts';
import { NodeIds, ZoneIds, ZoneWorldSpec } from '../Shared/spec';
import { readConfigPath, validateConfiguration } from '../Server/Configuration/configuration';
import type {
  AnnounceWorldRes,
  JoinWorldRes,
  MoveRejectedNotify,
  NodeStatusNotify,
  WatchNodesRes,
  WorldAnnounceNotify,
  ZoneChangedNotify,
  ZoneStateNotify
} from '../Shared/contracts';

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
    const joined = await gateway
      .request(new JoinWorldReq('player-a1'))
      .packetName(PacketNames.joinWorldReq)
      .submit<JoinWorldRes>();
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
    const joinedSecond = await second
      .request(new JoinWorldReq('player-a3'))
      .packetName(PacketNames.joinWorldReq)
      .submit<JoinWorldRes>();
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

    console.log('scenario ZW-B2 preparing cross-node move');
    position = await walkTo(gateway, joined.playerId, position, 49, 25);
    console.log('scenario ZW-B2 source position ready');
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
    console.log('scenario ZW-B2 target position ready');
    console.log('scenario ZW-B2 passed');

    await westObserver.connect();
    const westJoined = await westObserver
      .request(new JoinWorldReq('player-b1-west'))
      .packetName(PacketNames.joinWorldReq)
      .submit<JoinWorldRes>();
    await walkTo(
      westObserver,
      westJoined.playerId,
      { x: westJoined.x, y: westJoined.y },
      45,
      25
    );
    const adjacentView = await westObserver
      .waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) => player.playerId === joined.playerId))
      .timeout(10_000)
      .submit();
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
