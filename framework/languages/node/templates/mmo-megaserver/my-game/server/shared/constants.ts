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
  initialSequence: 0n,
  maxSafeInteger: 9_007_199_254_740_991n,
  maxStep: 10,
  maxSpeed: 50,
  regionMinX: 0,
  regionMaxX: 100,
  maxReplicasPerRegion: 2,
  allowedRegions: ['forest', 'desert'] as const
} as const;
export const Rejection = {
  regionFull: 'region-full',
  packetNotAllowed: 'packet-not-allowed',
  nonFinite: 'non-finite',
  stepTooLarge: 'step-too-large',
  outsideRegion: 'outside-region',
  moveBudget: 'move-budget',
  staleSequence: 'stale-sequence',
  integerOutOfRange: 'integer-out-of-range',
  regionUnknown: 'region-unknown',
  capacityExhausted: 'capacity-exhausted'
} as const;
const messageFollowMs = 30_000;
const relocationMs = 25_000;
export const Limits = {
  messageFollowMs,
  safeObservationMs: messageFollowMs + relocationMs,
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['region', 'movement', 'dungeon', 'reconnect', 'update'] as const;
export const PacketNames = {
  enterRegionRequest: 'EnterRegionRequest',
  joinRegion: 'JoinRegion',
  regionFull: 'RegionFull',
  regionUnavailable: 'RegionUnavailable',
  moveRequest: 'MoveRequest',
  actionRejected: 'ActionRejected',
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
