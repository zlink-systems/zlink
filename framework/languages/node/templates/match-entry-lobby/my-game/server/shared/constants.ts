export const Names = {
  mesh: 'my-game-arena',
  stream: 'players',
  actor: 'Player',
  room: 'Room',
  matchmaking: 'Matchmaking',
  matchmakingId: 'matchmaking',
  defaultMode: 'duel',
  roomPrefix: 'room-'
} as const;
export const Game = {
  maxSafeInteger: 9_007_199_254_740_991,
  playersPerRoom: 2,
  initialHp: 100,
  shotDamage: 25,
  shotRange: 80,
  mapMinX: 0,
  mapMaxX: 100,
  mapMinY: 0,
  mapMaxY: 100,
  maxStep: 2.0,
  maxSpeed: 10.0,
  fireIntervalMs: 250,
  firstSpawnX: 40,
  firstSpawnY: 10,
  secondSpawnX: 60,
  secondSpawnY: 10,
  wallX: 50,
  wallMinY: 20,
  wallMaxY: 60,
  playingPhase: 'Playing',
  finishedPhase: 'Finished'
} as const;
export const Rejection = {
  outOfRange: 'out-of-range',
  blockedByWall: 'blocked-by-wall',
  noTarget: 'no-target',
  admissionRejected: 'admission-rejected',
  invalidMove: 'invalid-move',
  staleSequence: 'stale-sequence',
  matchFinished: 'match-finished',
  stepTooLarge: 'step-too-large',
  outsideMap: 'outside-map',
  moveBlockedByWall: 'move-blocked-by-wall',
  moveBudget: 'move-budget',
  fireTooSoon: 'fire-too-soon',
  integerOutOfRange: 'integer-out-of-range',
  packetNotAllowed: 'packet-not-allowed'
} as const;
const MessageFollowMs = 30_000;
const RelocationMs = 25_000;
export const Limits = {
  messageFollowMs: MessageFollowMs,
  safeMs: MessageFollowMs + RelocationMs,
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs: RelocationMs,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['hit', 'wall', 'input', 'reconnect', 'update'] as const;
export const PacketNames = {
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo',
  queueRequest: 'QueueRequest',
  roomAdmission: 'RoomAdmission',
  roomAssignment: 'RoomAssignment',
  matchFound: 'MatchFound',
  moveRequest: 'MoveRequest',
  moveResult: 'MoveResult',
  shootRequest: 'ShootRequest',
  shotResult: 'ShotResult',
  roomViewRequest: 'RoomViewRequest',
  roomView: 'RoomView',
  actionRejected: 'ActionRejected',
  matchEnded: 'MatchEnded'
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
  match: 'server/match/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;
