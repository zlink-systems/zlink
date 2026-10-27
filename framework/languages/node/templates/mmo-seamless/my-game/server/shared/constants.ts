export const Names = {
  mesh: 'my-game-world',
  stream: 'players',
  actor: 'Player',
  zone: 'Zone',
  dungeon: 'Dungeon',
  border: 'border'
} as const;
export const Game = {
  zoneWidth: 100,
  zoneCount: 6,
  borderRadius: 10,
  maxStep: 10,
  maxSpeed: 50,
  fullBudgetAfterMs: 200,
  maxSafeInteger: 9_007_199_254_740_991
} as const;
const messageFollowMs = 30_000;
const relocationMs = 25_000;
export const Limits = {
  messageFollowMs,
  safeMs: messageFollowMs + relocationMs,
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs,
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
  playerAppeared: 'PlayerAppeared',
  playerMoved: 'PlayerMoved',
  playerLeft: 'PlayerLeft',
  actionRejected: 'ActionRejected',
  openDungeonRequest: 'OpenDungeonRequest',
  dungeonOpened: 'DungeonOpened'
} as const;
export const Control = {
  replacementX: 0,
  vacateZone: 'vacate-zone',
  zoneVacated: 'ZONE_VACATED',
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
  return `zone-${Math.floor(x / Game.zoneWidth) + 1}`;
}
export function adjacentZones(id: string): string[] {
  const n = Number(id.slice('zone-'.length));
  return [n - 1, n + 1].filter((v) => v >= 1 && v <= Game.zoneCount).map((v) => `zone-${v}`);
}
export function borderTopic(source: string, target: string): string {
  return `${source}:${target}`;
}
