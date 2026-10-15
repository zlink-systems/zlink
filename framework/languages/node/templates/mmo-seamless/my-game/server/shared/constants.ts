export const Names = {
  mesh: 'my-game-world',
  stream: 'players',
  actor: 'Player',
  zone: 'Zone',
  dungeon: 'Dungeon',
  border: 'border',
  borderTimer: 'border-tick'
} as const;
export const Game = {
  zoneWidth: 100,
  zoneCount: 6,
  borderRadius: 10,
  tickMs: 100
} as const;
export const Limits = {
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs: 25_000,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['cross', 'border', 'reconnect', 'update'] as const;
export const PacketNames = {
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo',
  joinWorldRequest: 'JoinWorldRequest',
  moveRequest: 'MoveRequest',
  enterZone: 'EnterZone',
  worldState: 'WorldState',
  borderPresence: 'BorderPresence',
  borderVisible: 'BorderVisible',
  openDungeonRequest: 'OpenDungeonRequest',
  dungeonOpened: 'DungeonOpened'
} as const;
export const Control = {
  ready: 'READY ',
  drain: 'drain',
  shutdown: 'shutdown',
  requested: 'RELOCATION_REQUESTED',
  relocated: 'RELOCATED',
  safe: 'SAFE_TO_SHUTDOWN',
  stopped: 'STOPPED '
} as const;
export const NodeEntrypoints = {
  session: 'server/session/main.ts',
  zone: 'server/zone/main.ts',
  instance: 'server/instance/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;
export function zoneId(x: number): string {
  if (!Number.isInteger(x) || x < 0 || x >= Game.zoneWidth * Game.zoneCount)
    throw new Error('Position outside world');
  return `zone-${Math.floor(x / Game.zoneWidth) + 1}`;
}
export function adjacentZones(id: string): string[] {
  const n = Number(id.slice('zone-'.length));
  return [n - 1, n + 1].filter((v) => v >= 1 && v <= Game.zoneCount).map((v) => `zone-${v}`);
}
export function borderTopic(source: string, target: string): string {
  return `${source}:${target}`;
}
