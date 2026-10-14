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
  playersPerRoom: 2,
  initialHp: 100,
  shotDamage: 25,
  shotRange: 80,
  walls: [{ x: 50, minY: 0, maxY: 100 }]
} as const;
export const Rejection = {
  outOfRange: 'out-of-range',
  blockedByWall: 'blocked-by-wall',
  noTarget: 'no-target',
  admissionRejected: 'admission-rejected',
  invalidMove: 'invalid-move',
  staleSequence: 'stale-sequence',
  matchFinished: 'match-finished'
} as const;
export const Limits = {
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs: 25_000,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['hit', 'reconnect', 'update'] as const;
export const PacketNames = {
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo',
  queueRequest: 'QueueRequest',
  roomAdmission: 'RoomAdmission',
  roomAssignment: 'RoomAssignment',
  matchFound: 'MatchFound',
  moveRequest: 'MoveRequest',
  shootRequest: 'ShootRequest',
  roomViewRequest: 'RoomViewRequest',
  roomView: 'RoomView',
  shotRejected: 'ShotRejected',
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
