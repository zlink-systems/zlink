const ZoneIds = {
  northWest: 'zone-nw',
  northEast: 'zone-ne',
  southWest: 'zone-sw',
  southEast: 'zone-se'
} as const;

const NodeIds = {
  west: 'zone-node-1',
  east: 'zone-node-2',
  extra: 'zone-node-3'
} as const;

const BotIds = {
  northWestX: 'bot-nw-x',
  northWestY: 'bot-nw-y',
  northEastX: 'bot-ne-x',
  northEastY: 'bot-ne-y',
  southWestX: 'bot-sw-x',
  southWestY: 'bot-sw-y',
  southEastX: 'bot-se-x',
  southEastY: 'bot-se-y'
} as const;

const ZoneWorldSpec = {
  worldSize: 100,
  zoneSplit: 50,
  borderBand: 10,
  maxStepPerAxis: 5,
  spawnX: 25,
  spawnY: 25,
  tickPeriodMs: 100,
  botTickPeriodMs: 500,
  botStep: 3,
  botsPerZone: 2,
  botCount: 8,
  borderSnapshotExpiryTicks: 3,
  nodeStatusReportPeriodMs: 1000
} as const;

const ZoneWorldNames = {
  zoneMesh: 'zoneworld.zones',
  bridgeMesh: 'zoneworld.bridge',
  broadcastChannel: 'zoneworld.broadcast',
  reportChannel: 'zoneworld.report',
  announceTopic: 'world.announce',
  maintenanceTopic: 'world.maintenance',
  playerActorType: 'zoneworld.player',
  gatewayStreamNode: 'zoneworld.gateway',
  opsStreamNode: 'zoneworld.ops',
  opsChannel: (nodeId: string) => `zoneworld.ops.${nodeId}`,
  borderTopic: (fromZoneId: string, toZoneId: string) => `zone.border.${fromZoneId}.${toZoneId}`
} as const;

const MoveRejectReasons = {
  outOfRange: 'OutOfRange',
  tooFar: 'TooFar',
  diagonalCrossing: 'DiagonalCrossing',
  zoneMaintenance: 'ZoneMaintenance'
} as const;

const NodeAlertKinds = {
  timerHandlerFailed: 'TimerHandlerFailed',
  peersChanged: 'PeersChanged'
} as const;

const ZoneWorldErrors = {
  nodeUnavailable: 'NodeUnavailable'
} as const;

type ZoneId = typeof ZoneIds[keyof typeof ZoneIds];
type NodeId = typeof NodeIds[keyof typeof NodeIds];
type MoveRejectReason = typeof MoveRejectReasons[keyof typeof MoveRejectReasons];
type NodeAlertKind = typeof NodeAlertKinds[keyof typeof NodeAlertKinds];

function zoneOf(x: number, y: number): ZoneId {
  if (x < ZoneWorldSpec.zoneSplit) {
    return y < ZoneWorldSpec.zoneSplit ? ZoneIds.northWest : ZoneIds.southWest;
  }
  return y < ZoneWorldSpec.zoneSplit ? ZoneIds.northEast : ZoneIds.southEast;
}

function nodeOf(zoneId: ZoneId): NodeId {
  return zoneId === ZoneIds.northWest || zoneId === ZoneIds.southWest
    ? NodeIds.west
    : NodeIds.east;
}

function zonesOf(nodeId: string): readonly ZoneId[] {
  if (nodeId === NodeIds.west) return [ZoneIds.northWest, ZoneIds.southWest];
  if (nodeId === NodeIds.east) return [ZoneIds.northEast, ZoneIds.southEast];
  return [];
}

export {
  BotIds,
  MoveRejectReasons,
  NodeAlertKinds,
  NodeIds,
  ZoneIds,
  ZoneWorldNames,
  ZoneWorldErrors,
  ZoneWorldSpec,
  nodeOf,
  zoneOf,
  zonesOf
};
export type { MoveRejectReason, NodeAlertKind, NodeId, ZoneId };
