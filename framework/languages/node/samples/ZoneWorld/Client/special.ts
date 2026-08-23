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
  CreateFreshActorProbeReq,
  JoinWorldReq,
  MessageFollowProbeReq,
  MoveMsg,
  NodeDiagnosticsReq,
  PacketNames,
  RelocationPairReq,
  SetMaintenanceReq,
  WatchNodesReq
} from '../Shared/contracts';
import type {
  ActorLocationProbeRes,
  AnnounceWorldRes,
  CreateFreshActorProbeRes,
  JoinWorldRes,
  MessageFollowProbeRes,
  MoveRejectedNotify,
  NodeAlertNotify,
  NodeDiagnosticsRes,
  NodeStatusNotify,
  PlayerView,
  RelocationPairRes,
  SetMaintenanceRes,
  WatchNodesRes,
  ZoneChangedNotify,
  ZoneStateNotify
} from '../Shared/contracts';
import {
  BotIds,
  MoveRejectReasons,
  NodeAlertKinds,
  NodeIds,
  ZoneIds,
  ZoneWorldErrors,
  ZoneWorldSpec,
  zoneOf
} from '../Shared/spec';
import { readConfigPath, validateConfiguration } from '../Server/Configuration/configuration';
import { joinAndWaitForOwnedState } from './join-readiness';

async function main(): Promise<void> {
  const path = readConfigPath(process.argv.slice(2));
  const config = validateConfiguration(JSON.parse(fs.readFileSync(path, 'utf8')) as unknown, 'client');
  if (config.client === undefined) throw new Error('Client configuration is required.');
  const scenario = config.client.scenarios;
  if (scenario === undefined) throw new Error('A special scenario name is required.');
  if (scenario === 'B4-C2-C3') await runFailureTransition(config.client.gatewayEndpoint, config.client.opsEndpoint);
  else if (scenario === 'LAYOUT') await runLayoutProbe(config.client.opsEndpoint);
  else if (scenario === 'PAIR') await runOpsProbe(config.client.opsEndpoint);
  else if (scenario === 'C4') await runSpotAlert(config.client.opsEndpoint);
  else if (scenario === 'D2') await runExtraSubscriber(config.client.opsEndpoint);
  else if (scenario === 'E') await runMaintenance(config.client.gatewayEndpoint, config.client.opsEndpoint);
  else if (scenario === 'E5-arm') await runMaintenanceArm(
    config.client.opsEndpoint,
    config.client.targetNodeId ?? NodeIds.east
  );
  else if (scenario === 'E5') await runMaintenanceRestore(
    config.client.opsEndpoint,
    config.client.targetNodeId ?? NodeIds.east
  );
  else if (scenario === 'G3' || scenario === 'G4') await runReplacementCreation(
    config.client.gatewayEndpoint,
    config.client.opsEndpoint,
    scenario,
    config.client.targetNodeId ?? NodeIds.east
  );
  else if (scenario === 'F') await runBots(config.client.gatewayEndpoint, config.client.opsEndpoint);
  else throw new Error(`Unknown special scenario '${scenario}'.`);
}

async function runFailureTransition(gatewayEndpoint: string, opsEndpoint: string): Promise<void> {
  const source = connector(gatewayEndpoint);
  const target = connector(gatewayEndpoint);
  const ops = connector(opsEndpoint);
  try {
    await Promise.all([source.connect(), target.connect(), ops.connect()]);
    const pair = await relocationPair(ops);
    const boundary = boundaryRoute(pair.targetZoneId);
    const nodes = await watch(ops);
    const targetNode = requireZoneOwner(nodes, pair.targetZoneId);
    const sourceJoin = await joinAndWaitForOwnedState(source, 'player-b4-west');
    const targetJoin = await joinAndWaitForOwnedState(target, 'player-b4-east');
    await walkTo(source, sourceJoin.playerId, sourceJoin, boundary.observer.x, boundary.observer.y);
    await walkTo(target, targetJoin.playerId, targetJoin, boundary.targetInside.x, boundary.targetInside.y);
    await source.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) =>
        player.playerId === targetJoin.playerId && player.zoneId === pair.targetZoneId))
      .timeout(20_000).submit();
    zlinkStreamAssert.ensure(targetNode.registered, 'ZW-C3 did not begin from Registered=true.');
    zlinkStreamAssert.ensure(targetNode.connected, 'ZW-C2 did not begin from Connected=true.');
    const expired = source.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => !message.payload.players.some((player) => player.zoneId === pair.targetZoneId))
      .timeout(60_000).submit();
    const unregistered = ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
      .where((message) => message.payload.nodeId === targetNode.nodeId && !message.payload.registered)
      .timeout(60_000).submit();
    const disconnected = ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
      .where((message) => message.payload.nodeId === targetNode.nodeId && !message.payload.connected)
      .timeout(60_000).submit();
    console.log(`scenario ZW-B4-C2-C3 armed node=${targetNode.nodeId}`);
    await Promise.all([
      withScenarioContext('ZW-B4 border snapshot expiry', expired),
      withScenarioContext('ZW-C2 runtime disconnected status', disconnected),
      withScenarioContext('ZW-C3 report TTL expired status', unregistered)
    ]);
    const previousOwnerTerminal = await target
      .request(new MessageFollowProbeReq(targetJoin.playerId, 'zw-g4-crashed-owner', 'crash-boundary'))
      .packetName(PacketNames.messageFollowProbeReq)
      .submit<MessageFollowProbeRes>();
    zlinkStreamAssert.ensure(
      previousOwnerTerminal.error === ZoneWorldErrors.actorUnavailable,
      'ZW-G4 previous-owner operation did not end at the Unavailable boundary.'
    );
    console.log(`crash-boundary=${previousOwnerTerminal.error} actor=${targetJoin.playerId}`);
    console.log('scenario ZW-B4 passed');
    console.log('scenario ZW-C2 passed');
    console.log('scenario ZW-C3 passed');
  } finally {
    await closeAll(source, target, ops);
  }
}

async function runOpsProbe(opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    const nodes = await watch(ops);
    const pair = await relocationPair(ops);
    console.log(`ops-probe=${JSON.stringify({ nodes: nodes.nodes, pair })}`);
  } finally {
    await closeAll(ops);
  }
}

async function runLayoutProbe(opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    const nodes = await watch(ops);
    console.log(`ops-layout=${JSON.stringify(nodes.nodes)}`);
  } finally {
    await closeAll(ops);
  }
}

async function runSpotAlert(opsEndpoint: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    const alert = ops.waitFor<NodeAlertNotify>(PacketNames.nodeAlertNotify)
      .where((message) => message.payload.kind === NodeAlertKinds.timerHandlerFailed)
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
  try {
    const nodes = await watch(ops);
    const pair = await relocationPair(ops);
    const sourceNode = requireZoneOwner(nodes, pair.sourceZoneId);
    const targetNode = requireZoneOwner(nodes, pair.targetZoneId);
    await resetMaintenance(ops);
    await verifyTargetIsolation(ops, targetNode);
    await verifyMaintainedLocalMovement(gatewayEndpoint, ops, pair.sourceZoneId, sourceNode);
    await verifyNewJoinRejection(gatewayEndpoint, ops, sourceNode.nodeId);
    const diagnostics = await diagnose(ops, targetNode.nodeId);
    zlinkStreamAssert.ensure(
      diagnostics.zones.join(',') === targetNode.zones.join(','),
      'ZW-E6 zone list did not match the Ops layout.'
    );
    zlinkStreamAssert.ensure(diagnostics.playerCount >= 0, 'ZW-E6 player count was negative.');
    console.log('scenario ZW-E6 passed');
  } finally {
    await resetMaintenance(ops);
    await closeAll(ops);
  }
}

async function verifyTargetIsolation(
  ops: ZlinkStreamConnector,
  targetNode: WatchNodesRes['nodes'][number]
): Promise<void> {
  try {
    const applied = await setMaintenance(ops, targetNode.nodeId, true);
    zlinkStreamAssert.ensure(
      applied.zones.join(',') === targetNode.zones.join(','),
      'ZW-E1 maintenance response did not use the Ops-discovered target layout.'
    );
    console.log('scenario ZW-E1 passed');
  } finally {
    await setMaintenance(ops, targetNode.nodeId, false);
  }
}

async function verifyMaintainedLocalMovement(
  gatewayEndpoint: string,
  ops: ZlinkStreamConnector,
  sourceZoneId: string,
  sourceNode: WatchNodesRes['nodes'][number]
): Promise<void> {
  const game = connector(gatewayEndpoint);
  try {
    await game.connect();
    const joined = await joinAndWaitForOwnedState(game, 'player-e1');
    const sameOwnerTarget = sourceNode.zones.find((zoneId) =>
      zoneId !== sourceZoneId && (zoneId === ZoneIds.northEast || zoneId === ZoneIds.southWest));
    if (sameOwnerTarget === undefined) {
      throw new Error(`ZW-E4 Ops layout has no adjacent same-owner zone for '${sourceZoneId}'.`);
    }
    const boundary = boundaryRoute(sameOwnerTarget);
    await walkTo(game, joined.playerId, joined, boundary.sourceEdge.x, boundary.sourceEdge.y);
    await setMaintenance(ops, sourceNode.nodeId, true);
    await expectMaintenanceRejection(game, boundary.targetInside.x, boundary.targetInside.y);
    console.log('scenario ZW-E4 passed');
    await moveAndWait(game, joined.playerId, boundary.observer.x, boundary.observer.y);
    console.log('scenario ZW-E3 passed');
  } finally {
    await setMaintenance(ops, sourceNode.nodeId, false);
    await closeAll(game);
  }
}

async function verifyNewJoinRejection(
  gatewayEndpoint: string,
  ops: ZlinkStreamConnector,
  sourceNodeId: string
): Promise<void> {
  const newcomer = connector(gatewayEndpoint);
  try {
    await setMaintenance(ops, sourceNodeId, true);
    await newcomer.connect();
    const rejected = await join(newcomer, 'player-e3');
    zlinkStreamAssert.ensure(rejected.error === MoveRejectReasons.zoneMaintenance, 'ZW-E2 entry was not rejected.');
    console.log('scenario ZW-E2 passed');
  } finally {
    await setMaintenance(ops, sourceNodeId, false);
    await closeAll(newcomer);
  }
}

async function runMaintenanceArm(opsEndpoint: string, targetNodeId: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    const nodes = await watch(ops);
    const target = nodes.nodes.find((node) => node.nodeId === targetNodeId);
    if (target?.registered !== true || target.connected !== true) {
      await ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
        .where((message) => message.payload.nodeId === targetNodeId
          && message.payload.registered
          && message.payload.connected)
        .timeout(20_000)
        .submit();
    }
    const applied = await setMaintenance(ops, targetNodeId, true);
    zlinkStreamAssert.ensure(applied.error === null, 'ZW-E5 could not arm maintenance.');
    console.log('scenario ZW-E5 armed');
  } finally {
    await closeAll(ops);
  }
}

async function runMaintenanceRestore(opsEndpoint: string, targetNodeId: string): Promise<void> {
  const ops = connector(opsEndpoint);
  try {
    await ops.connect();
    const nodes = await watch(ops);
    const target = nodes.nodes.find((node) => node.nodeId === targetNodeId);
    if (target?.registered !== true || target.connected !== true) {
      await ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
        .where((message) => message.payload.nodeId === targetNodeId
          && message.payload.registered
          && message.payload.connected)
        .timeout(20_000)
        .submit();
    }
    const diagnostics = await diagnose(ops, targetNodeId);
    zlinkStreamAssert.ensure(
      diagnostics.error === null,
      `ZW-E5 Ops could not reach '${targetNodeId}' to read its maintenance state.`
    );
    zlinkStreamAssert.ensure(diagnostics.maintenance, 'ZW-E5 maintenance state was not restored.');
    console.log('scenario ZW-E5 passed');
    await setMaintenance(ops, targetNodeId, false);
  } finally {
    await closeAll(ops);
  }
}

async function runReplacementCreation(
  gatewayEndpoint: string,
  opsEndpoint: string,
  scenario: 'G3' | 'G4',
  targetNodeId: string
): Promise<void> {
  const game = connector(gatewayEndpoint);
  const ops = connector(opsEndpoint);
  try {
    await Promise.all([game.connect(), ops.connect()]);
    const emptyReport = await waitForNodeReport(
      ops,
      targetNodeId,
      (node) => node.registered && node.connected && node.zones.length === 0
    );
    zlinkStreamAssert.ensure(
      emptyReport.zones.length === 0,
      `ZW-${scenario} replacement did not start with zero local zones.`
    );
    for (let index = 0; index < 16; index += 1) {
      const actorId = `player-${scenario.toLowerCase()}-fresh-${index}`;
      const created = await game
        .request(new CreateFreshActorProbeReq(actorId))
        .packetName(PacketNames.createFreshActorProbeReq)
        .submit<CreateFreshActorProbeRes>();
      zlinkStreamAssert.ensure(created.error === null, `ZW-${scenario} fresh Actor creation failed.`);
      const located = await game
        .request(new ActorLocationProbeReq(actorId))
        .packetName(PacketNames.actorLocationProbeReq)
        .submit<ActorLocationProbeRes>();
      zlinkStreamAssert.ensure(
        located.error === null
          && located.actorId === created.actorId
          && located.objectGeneration === created.objectGeneration
          && located.nodeRid === created.nodeRid,
        `ZW-${scenario} fresh Actor routing probe did not match creation.`
      );
      console.log(`fresh-actor-proof=${JSON.stringify({
        scenario,
        nodeId: emptyReport.nodeId,
        actorId: created.actorId,
        objectGeneration: created.objectGeneration,
        nodeRid: created.nodeRid
      })}`);
    }
  } finally {
    await closeAll(game, ops);
  }
}

async function waitForNodeReport(
  ops: ZlinkStreamConnector,
  nodeId: string,
  predicate: (node: WatchNodesRes['nodes'][number]) => boolean
): Promise<WatchNodesRes['nodes'][number]> {
  const current = (await watch(ops)).nodes.find((node) => node.nodeId === nodeId);
  if (current !== undefined && predicate(current)) return current;
  const observed = await ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
    .where((message) => message.payload.nodeId === nodeId && predicate(message.payload))
    .timeout(20_000)
    .submit();
  return observed.payload;
}

async function runBots(gatewayEndpoint: string, opsEndpoint: string): Promise<void> {
  const game = connector(gatewayEndpoint);
  const ops = connector(opsEndpoint);
  try {
    await Promise.all([game.connect(), ops.connect()]);
    await watch(ops);
    const joined = await joinAndWaitForOwnedState(game, 'player-f');
    await resetMaintenance(ops);
    await setMaintenance(ops, NodeIds.east, true);
    await verifyRepresentativeBotMovement(game);
    console.log('scenario ZW-F1 passed');
    await ops.request(new AnnounceWorldReq('bot-path-check')).packetName(PacketNames.announceWorldReq).submit();
    await expectRejected(game, -40, joined.y, MoveRejectReasons.outOfRange);
    await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) => player.isBot)).submit();
    console.log('scenario ZW-F3 passed');
    const peak = await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
      .where((message) => message.payload.players.some((player) =>
        player.playerId === BotIds.northEastX && player.zoneId === ZoneIds.northWest && player.x >= 46))
      .timeout(60_000).submit();
    const candidate = peak.payload.players.find((player) =>
      player.playerId === BotIds.northEastX && player.zoneId === ZoneIds.northWest && player.x >= 46);
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

async function verifyRepresentativeBotMovement(game: ZlinkStreamConnector): Promise<void> {
  const representatives = [BotIds.northEastX, BotIds.southWestY] as const;
  const initial = await game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => representatives.every((playerId) => message.payload.players.some((player) =>
      player.playerId === playerId && player.zoneId === ZoneIds.northWest)))
    .timeout(30_000)
    .submit();
  const xBot = requirePlayer(initial.payload.players, BotIds.northEastX);
  const yBot = requirePlayer(initial.payload.players, BotIds.southWestY);
  const xMoved = game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => message.payload.players.some((player) =>
      player.playerId === xBot.playerId && player.x !== xBot.x && player.y === xBot.y))
    .timeout(30_000)
    .submit();
  const yMoved = game.waitFor<ZoneStateNotify>(PacketNames.zoneStateNotify)
    .where((message) => message.payload.players.some((player) =>
      player.playerId === yBot.playerId && player.x === yBot.x && player.y !== yBot.y))
    .timeout(30_000)
    .submit();
  const [xState, yState] = await Promise.all([xMoved, yMoved]);
  assertBotAxisStep(requirePlayer(xState.payload.players, xBot.playerId), xBot, 'x');
  assertBotAxisStep(requirePlayer(yState.payload.players, yBot.playerId), yBot, 'y');
}

function requirePlayer(players: readonly PlayerView[], playerId: string): PlayerView {
  const player = players.find((candidate) => candidate.playerId === playerId);
  if (player === undefined) throw new Error(`Bot '${playerId}' disappeared from the observed zone.`);
  return player;
}

function assertBotAxisStep(current: PlayerView, previous: PlayerView, axis: 'x' | 'y'): void {
  const distance = Math.abs(current[axis] - previous[axis]);
  zlinkStreamAssert.ensure(
    distance >= ZoneWorldSpec.botStep && distance % ZoneWorldSpec.botStep === 0,
    `Bot '${current.playerId}' did not follow the deterministic ${axis.toUpperCase()} patrol step.`
  );
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
  const reply = client.waitFor<JoinWorldRes>(PacketNames.joinWorldRes)
    .where((message) => message.payload.playerId === playerId)
    .timeout(10_000)
    .submit();
  await client.send(new JoinWorldReq(playerId)).packetName(PacketNames.joinWorldReq).submit();
  return (await reply).payload;
}

async function watch(ops: ZlinkStreamConnector): Promise<WatchNodesRes> {
  return await ops.request(new WatchNodesReq()).packetName(PacketNames.watchNodesReq).submit<WatchNodesRes>();
}

async function relocationPair(ops: ZlinkStreamConnector): Promise<RelocationPairRes> {
  const pair = await ops.request(new RelocationPairReq())
    .packetName(PacketNames.relocationPairReq).submit<RelocationPairRes>();
  zlinkStreamAssert.ensure(pair.error === null, 'Ops did not report a cross-owner adjacent zone pair.');
  return pair;
}

function requireZoneOwner(nodes: WatchNodesRes, zoneId: string): WatchNodesRes['nodes'][number] {
  const owner = nodes.nodes.find((node) => node.registered && node.zones.includes(zoneId));
  if (owner === undefined) throw new Error(`Ops did not report an owner for '${zoneId}'.`);
  return owner;
}

function boundaryRoute(targetZoneId: string) {
  if (targetZoneId === ZoneIds.northEast) {
    return {
      observer: { x: 45, y: 25 }, sourceEdge: { x: 49, y: 25 }, targetInside: { x: 52, y: 25 }
    } as const;
  }
  if (targetZoneId === ZoneIds.southWest) {
    return {
      observer: { x: 25, y: 45 }, sourceEdge: { x: 25, y: 49 }, targetInside: { x: 25, y: 52 }
    } as const;
  }
  throw new Error(`Unsupported Ops-selected target zone '${targetZoneId}'.`);
}

async function diagnose(ops: ZlinkStreamConnector, nodeId: string): Promise<NodeDiagnosticsRes> {
  return await ops.request(new NodeDiagnosticsReq(nodeId))
    .packetName(PacketNames.nodeDiagnosticsReq).submit<NodeDiagnosticsRes>();
}

async function setMaintenance(ops: ZlinkStreamConnector, nodeId: string, enabled: boolean): Promise<SetMaintenanceRes> {
  const response = await ops.request(new SetMaintenanceReq(nodeId, enabled))
    .packetName(PacketNames.setMaintenanceReq).submit<SetMaintenanceRes>();
  //  Ops commits the desired state before it tries the owner-consistent
  //  channel to the node, so `nodeUnavailable` still means the state was
  //  recorded - a node that is between transport connections reads it back
  //  when it reconnects. Only that answer carries no status notification, so
  //  the observation below applies to the reachable case.
  zlinkStreamAssert.ensure(
    response.error === null || response.error === ZoneWorldErrors.nodeUnavailable,
    `Maintenance request for '${nodeId}' failed.`
  );
  if (response.error !== null) return response;
  const observed = ops.waitFor<NodeStatusNotify>(PacketNames.nodeStatusNotify)
    .where((message) => message.payload.nodeId === nodeId && message.payload.maintenance === enabled)
    .timeout(20_000).submit();
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
