import * as fs from 'node:fs';
import {
  ZlinkStreamDispatchMode,
  zlinkStreamAssert,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import {
  AnnounceWorldReq,
  JoinWorldReq,
  MoveMsg,
  NodeDiagnosticsReq,
  PacketNames,
  SetMaintenanceReq,
  WatchNodesReq
} from '../Shared/contracts';
import type {
  AnnounceWorldRes,
  JoinWorldRes,
  MoveRejectedNotify,
  NodeAlertNotify,
  NodeDiagnosticsRes,
  NodeStatusNotify,
  SetMaintenanceRes,
  WatchNodesRes,
  ZoneChangedNotify,
  ZoneStateNotify
} from '../Shared/contracts';
import { MoveRejectReasons, NodeAlertKinds, NodeIds, ZoneIds, zoneOf } from '../Shared/spec';
import { readConfigPath, validateConfiguration } from '../Server/Configuration/configuration';

async function main(): Promise<void> {
  const path = readConfigPath(process.argv.slice(2));
  const config = validateConfiguration(JSON.parse(fs.readFileSync(path, 'utf8')) as unknown, 'client');
  if (config.client === undefined) throw new Error('Client configuration is required.');
  const scenario = config.client.scenarios;
  if (scenario === undefined) throw new Error('A special scenario name is required.');
  if (scenario === 'B4-C2-C3') await runFailureTransition(config.client.gatewayEndpoint, config.client.opsEndpoint);
  else if (scenario === 'C4') await runSpotAlert(config.client.opsEndpoint);
  else if (scenario === 'D2') await runExtraSubscriber(config.client.opsEndpoint);
  else if (scenario === 'E') await runMaintenance(config.client.gatewayEndpoint, config.client.opsEndpoint);
  else if (scenario === 'E5-arm') await runMaintenanceArm(config.client.opsEndpoint);
  else if (scenario === 'E5') await runMaintenanceRestore(config.client.opsEndpoint);
  else if (scenario === 'F') await runBots(config.client.gatewayEndpoint, config.client.opsEndpoint);
  else throw new Error(`Unknown special scenario '${scenario}'.`);
}

async function runFailureTransition(gatewayEndpoint: string, opsEndpoint: string): Promise<void> {
  const west = connector(gatewayEndpoint);
  const east = connector(gatewayEndpoint);
  const ops = connector(opsEndpoint);
  try {
    await Promise.all([west.connect(), east.connect(), ops.connect()]);
    const westJoin = await join(west, 'player-b4-west');
    const eastJoin = await join(east, 'player-b4-east');
    await walkTo(west, westJoin.playerId, westJoin, 45, 25);
    await walkTo(east, eastJoin.playerId, eastJoin, 52, 25);
    await west.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) =>
        player.playerId === eastJoin.playerId && player.zoneId === ZoneIds.northEast))
      .timeout(20_000).submit();
    const nodes = await watch(ops);
    const eastNode = nodes.nodes.find((node) => node.nodeId === NodeIds.east);
    zlinkStreamAssert.ensure(eastNode?.registered === true, 'ZW-C2 did not begin from Registered=true.');
    zlinkStreamAssert.ensure(eastNode.connected, 'ZW-C3 did not begin from Connected=true.');
    const expired = west.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => !message.payload.players.some((player) => player.zoneId === ZoneIds.northEast))
      .timeout(60_000).submit();
    const unregistered = ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
      .where((message) => message.payload.nodeId === NodeIds.east && !message.payload.registered)
      .timeout(60_000).submit();
    const disconnected = ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
      .where((message) => message.payload.nodeId === NodeIds.east && !message.payload.connected)
      .timeout(60_000).submit();
    console.log('scenario ZW-B4-C2-C3 armed');
    await Promise.all([
      withScenarioContext('ZW-B4 border snapshot expiry', expired),
      withScenarioContext('ZW-C2 east node unregistered status', unregistered),
      withScenarioContext('ZW-C3 east node disconnected status', disconnected)
    ]);
    console.log('scenario ZW-B4 passed');
    console.log('scenario ZW-C2 passed');
    console.log('scenario ZW-C3 passed');
  } finally {
    await closeAll(west, east, ops);
  }
}

async function runSpotAlert(opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    const alert = ops.waitFor<NodeAlertNotify>(PacketNames.nodeAlertNotify)
      .where((message) => message.payload.nodeId === NodeIds.west
        && message.payload.kind === NodeAlertKinds.timerHandlerFailed)
      .timeout(60_000).submit();
    await watch(ops);
    console.log('scenario ZW-C4 armed');
    await alert;
    console.log('scenario ZW-C4 passed');
  } finally {
    await closeAll(ops);
  }
}

async function runExtraSubscriber(opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    await watch(ops);
    await ops.request(new AnnounceWorldReq('extra-node-ready'))
      .packetName(PacketNames.announceWorldReq).submit<AnnounceWorldRes>();
    console.log('scenario ZW-D2 published');
  } finally {
    await closeAll(ops);
  }
}

async function runMaintenance(gatewayEndpoint: string, opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  await ops.connect();
  await watch(ops);
  try {
    await resetMaintenance(ops);
    await verifyEastIsolation(gatewayEndpoint, ops);
    await verifyExistingPlayer(gatewayEndpoint, ops);
    await verifyDeparture(gatewayEndpoint, ops);
    const diagnostics = await diagnose(ops, NodeIds.west);
    zlinkStreamAssert.ensure(
      diagnostics.zones.join(',') === [ZoneIds.northWest, ZoneIds.southWest].join(','),
      'ZW-E4 zone list mismatch.'
    );
    zlinkStreamAssert.ensure(diagnostics.playerCount >= 0, 'ZW-E4 player count was negative.');
    console.log('scenario ZW-E4 passed');
    await setMaintenance(ops, NodeIds.west, true);
    const newcomer = connector(gatewayEndpoint);
    try {
      await newcomer.connect();
      const rejected = await join(newcomer, 'player-e6');
      zlinkStreamAssert.ensure(rejected.error === MoveRejectReasons.zoneMaintenance, 'ZW-E6 entry was not rejected.');
      console.log('scenario ZW-E6 passed');
    } finally {
      await closeAll(newcomer);
    }
  } finally {
    await resetMaintenance(ops);
    await closeAll(ops);
  }
}

async function verifyEastIsolation(gatewayEndpoint: string, ops: ZlinkStreamConnector): Promise<void> {
  const game = connector(gatewayEndpoint);
  try {
    await game.connect();
    const joined = await join(game, 'player-e1');
    let position = await walkTo(game, joined.playerId, joined, 48, 25);
    const applied = await setMaintenance(ops, NodeIds.east, true);
    zlinkStreamAssert.ensure(
      applied.zones.join(',') === [ZoneIds.northEast, ZoneIds.southEast].join(','),
      'ZW-E1 did not target both east zones.'
    );
    await expectMaintenanceRejection(game, 52, 25);
    position = await walkTo(game, joined.playerId, position, 48, 55);
    await expectMaintenanceRejection(game, 52, 55);
    await moveAndWait(game, joined.playerId, 45, 55);
    console.log('scenario ZW-E1 passed');
  } finally {
    await setMaintenance(ops, NodeIds.east, false);
    await closeAll(game);
  }
}

async function verifyExistingPlayer(gatewayEndpoint: string, ops: ZlinkStreamConnector): Promise<void> {
  const game = connector(gatewayEndpoint);
  try {
    await game.connect();
    const joined = await join(game, 'player-e2');
    await setMaintenance(ops, NodeIds.west, true);
    await moveAndWait(game, joined.playerId, 30, 30);
    await walkTo(game, joined.playerId, { x: 30, y: 30 }, 30, 48);
    const changed = game.waitFor<ZoneChangedNotify>(PacketNames.zoneChangedNotify)
      .where((message) => message.payload.zoneId === ZoneIds.southWest).submit();
    await game.send(new MoveMsg(30, 52)).packetName(PacketNames.moveMsg).submit();
    zlinkStreamAssert.ensure(!(await changed).payload.transferred, 'ZW-E2 node-local move transferred the actor.');
    console.log('scenario ZW-E2 passed');
  } finally {
    await setMaintenance(ops, NodeIds.west, false);
    await closeAll(game);
  }
}

async function verifyDeparture(gatewayEndpoint: string, ops: ZlinkStreamConnector): Promise<void> {
  const game = connector(gatewayEndpoint);
  try {
    await game.connect();
    const joined = await join(game, 'player-e3');
    await walkTo(game, joined.playerId, joined, 48, 25);
    await setMaintenance(ops, NodeIds.west, true);
    const changed = game.waitFor<ZoneChangedNotify>(PacketNames.zoneChangedNotify)
      .where((message) => message.payload.nodeId === NodeIds.east).submit();
    await game.send(new MoveMsg(52, 25)).packetName(PacketNames.moveMsg).submit();
    zlinkStreamAssert.ensure((await changed).payload.transferred, 'ZW-E3 departure did not transfer the actor.');
    console.log('scenario ZW-E3 passed');
  } finally {
    await setMaintenance(ops, NodeIds.west, false);
    await closeAll(game);
  }
}

async function runMaintenanceArm(opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    const nodes = await watch(ops);
    const east = nodes.nodes.find((node) => node.nodeId === NodeIds.east);
    if (east?.registered !== true || east.connected !== true) {
      await ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
        .where((message) => message.payload.nodeId === NodeIds.east
          && message.payload.registered
          && message.payload.connected)
        .timeout(20_000)
        .submit();
    }
    const applied = await setMaintenance(ops, NodeIds.east, true);
    zlinkStreamAssert.ensure(applied.error === null, 'ZW-E5 could not arm maintenance.');
    console.log('scenario ZW-E5 armed');
  } finally {
    await closeAll(ops);
  }
}

async function runMaintenanceRestore(opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    await watch(ops);
    const diagnostics = await diagnose(ops, NodeIds.east);
    zlinkStreamAssert.ensure(diagnostics.maintenance, 'ZW-E5 maintenance state was not restored.');
    console.log('scenario ZW-E5 passed');
    await setMaintenance(ops, NodeIds.east, false);
  } finally {
    await closeAll(ops);
  }
}

async function runBots(gatewayEndpoint: string, opsEndpoint: string): Promise<void> {
  const game = connector(gatewayEndpoint);
  const ops = connector(opsEndpoint);
  try {
    await Promise.all([game.connect(), ops.connect()]);
    await watch(ops);
    const joined = await join(game, 'player-f');
    const initial = await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) => player.isBot))
      .timeout(30_000).submit();
    const bots = new Map(initial.payload.players.filter((player) => player.isBot).map((bot) => [bot.playerId, `${bot.x},${bot.y}`]));
    const moved = new Set<string>();
    while (moved.size < bots.size) {
      const state = await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify).submit();
      for (const bot of state.payload.players.filter((player) => bots.has(player.playerId))) {
        if (bots.get(bot.playerId) !== `${bot.x},${bot.y}`) moved.add(bot.playerId);
      }
    }
    console.log('scenario ZW-F1 passed');
    await ops.request(new AnnounceWorldReq('bot-path-check')).packetName(PacketNames.announceWorldReq).submit();
    await expectRejected(game, -40, joined.y, MoveRejectReasons.outOfRange);
    await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) => player.isBot)).submit();
    console.log('scenario ZW-F3 passed');
    await resetMaintenance(ops);
    await setMaintenance(ops, NodeIds.east, true);
    const peak = await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) =>
        player.isBot && player.playerId.endsWith('-x') && player.zoneId === ZoneIds.northWest && player.x >= 46))
      .timeout(60_000).submit();
    const candidate = peak.payload.players.find((player) =>
      player.isBot && player.playerId.endsWith('-x') && player.zoneId === ZoneIds.northWest && player.x >= 46);
    if (candidate === undefined) throw new Error('ZW-F4 boundary bot disappeared.');
    await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) =>
        player.playerId === candidate.playerId && player.x < candidate.x))
      .timeout(60_000).submit();
    console.log('scenario ZW-F4 passed');
  } finally {
    await setMaintenance(ops, NodeIds.east, false).catch(() => undefined);
    await closeAll(game, ops);
  }
}

function connector(endpoint: string): ZlinkStreamConnector {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    waitTimeoutMs: 60_000,
    heartbeat: { enabled: false }
  });
}

async function join(client: ZlinkStreamConnector, playerId: string): Promise<JoinWorldRes> {
  return await client.request(new JoinWorldReq(playerId))
    .packetName(PacketNames.joinWorldReq).submit<JoinWorldRes>();
}

async function watch(ops: ZlinkStreamConnector): Promise<WatchNodesRes> {
  return await ops.request(new WatchNodesReq()).packetName(PacketNames.watchNodesReq).submit<WatchNodesRes>();
}

async function diagnose(ops: ZlinkStreamConnector, nodeId: string): Promise<NodeDiagnosticsRes> {
  return await ops.request(new NodeDiagnosticsReq(nodeId))
    .packetName(PacketNames.nodeDiagnosticsReq).submit<NodeDiagnosticsRes>();
}

async function setMaintenance(ops: ZlinkStreamConnector, nodeId: string, enabled: boolean): Promise<SetMaintenanceRes> {
  const observed = ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
    .where((message) => message.payload.nodeId === nodeId && message.payload.maintenance === enabled)
    .timeout(20_000).submit();
  const response = await ops.request(new SetMaintenanceReq(nodeId, enabled))
    .packetName(PacketNames.setMaintenanceReq).submit<SetMaintenanceRes>();
  zlinkStreamAssert.ensure(response.error === null, `Maintenance request for '${nodeId}' failed.`);
  await withScenarioContext(`maintenance status ${nodeId}=${enabled}`, observed);
  return response;
}

async function resetMaintenance(ops: ZlinkStreamConnector): Promise<void> {
  await setMaintenance(ops, NodeIds.west, false);
  await setMaintenance(ops, NodeIds.east, false);
}

async function expectMaintenanceRejection(client: ZlinkStreamConnector, x: number, y: number): Promise<void> {
  await expectRejected(client, x, y, MoveRejectReasons.zoneMaintenance);
}

async function expectRejected(
  client: ZlinkStreamConnector,
  x: number,
  y: number,
  reason: string
): Promise<void> {
  const rejected = client.waitFor<MoveRejectedNotify>(PacketNames.moveRejectedNotify)
    .where((message) => message.payload.reason === reason)
    .timeout(20_000).submit();
  await client.send(new MoveMsg(x, y)).packetName(PacketNames.moveMsg).submit();
  await withScenarioContext(`move rejection ${reason} at ${x},${y}`, rejected);
}

async function walkTo(
  client: ZlinkStreamConnector,
  playerId: string,
  from: { x: number; y: number },
  targetX: number,
  targetY: number
): Promise<{ x: number; y: number }> {
  let current = { x: from.x, y: from.y };
  while (current.x !== targetX || current.y !== targetY) {
    const next = {
      x: stepToward(current.x, targetX),
      y: stepToward(current.y, targetY)
    };
    if (zoneOf(current.x, current.y) === zoneOf(next.x, next.y)) {
      await moveAndWait(client, playerId, next.x, next.y);
    } else {
      await moveAcrossZoneAndWait(client, playerId, next.x, next.y);
    }
    current = next;
  }
  return current;
}

async function moveAcrossZoneAndWait(
  client: ZlinkStreamConnector,
  playerId: string,
  x: number,
  y: number
): Promise<void> {
  const targetZone = zoneOf(x, y);
  const changed = client.waitFor<ZoneChangedNotify>(PacketNames.zoneChangedNotify)
    .where((message) => message.payload.playerId === playerId && message.payload.zoneId === targetZone)
    .timeout(20_000).submit();
  const settled = client.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => message.payload.zoneId === targetZone && message.payload.players.some((player) =>
      player.playerId === playerId && player.x === x && player.y === y))
    .timeout(20_000).submit();
  await client.send(new MoveMsg(x, y)).packetName(PacketNames.moveMsg).submit();
  await withScenarioContext(`player ${playerId} zone ${targetZone}`, Promise.all([changed, settled]));
}

async function moveAndWait(client: ZlinkStreamConnector, playerId: string, x: number, y: number): Promise<void> {
  const observed = client.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => message.payload.players.some((player) =>
      player.playerId === playerId && player.x === x && player.y === y))
    .timeout(20_000).submit();
  await client.send(new MoveMsg(x, y)).packetName(PacketNames.moveMsg).submit();
  await withScenarioContext(`player ${playerId} position ${x},${y}`, observed);
}

async function withScenarioContext<T>(description: string, observed: Promise<T>): Promise<T> {
  try {
    return await observed;
  } catch (error) {
    throw new Error(`Timed out or failed while waiting for ${description}.`, { cause: error });
  }
}

function stepToward(value: number, target: number): number {
  return value === target ? value : value + Math.sign(target - value) * Math.min(5, Math.abs(target - value));
}

async function closeAll(...clients: ZlinkStreamConnector[]): Promise<void> {
  await Promise.allSettled(clients.map((client) => client.close()));
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
