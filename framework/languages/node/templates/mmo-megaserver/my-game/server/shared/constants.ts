export const Names = {
  mesh: 'my-game-world',
  stream: 'players',
  actor: 'Player',
  region: 'Region',
  dungeon: 'Dungeon'
} as const;
export const Game = {
  firstIndex: 1,
  initialX: 0,
  initialSequence: 0
} as const;
export const Rejection = { regionFull: 'region-full' } as const;
export const Limits = {
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs: 25_000,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['region', 'dungeon', 'reconnect', 'update'] as const;
export const PacketNames = {
  enterRegionRequest: 'EnterRegionRequest',
  joinRegion: 'JoinRegion',
  regionFull: 'RegionFull',
  moveRequest: 'MoveRequest',
  regionView: 'RegionView',
  viewRequest: 'ViewRequest',
  openDungeonRequest: 'OpenDungeonRequest',
  dungeonInfoRequest: 'DungeonInfoRequest',
  dungeonOpened: 'DungeonOpened',
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo'
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
  region: 'server/region/main.ts',
  instance: 'server/instance/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;
export function regionId(region: string, index: number): string {
  return `${region}-${index}`;
}
