export const Names = {
  mesh: 'my-game-arena',
  stream: 'players',
  actor: 'Player',
  room: 'Room',
  defaultMode: 'duel',
  roomPrefix: 'room-',
  matchPath: '/match'
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
const messageFollowMs = 20_000;
const messageMs = 10_000;
export const Limits = {
  messageFollowMs,
  observationCapacity: 64,
  messageMs,
  startupMs: 30_000,
  relocationMs: messageFollowMs + messageMs,
  shutdownMs: 10_000
} as const;
export const Scenarios = ['lobby', 'hit', 'reconnect', 'update'] as const;
export const PacketNames = {
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo',
  joinRoomRequest: 'JoinRoomRequest',
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
  match: 'server/match/main.ts',
  'http-lobby': 'server/lobby/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;

export const Http = {
  ok: 200,
  badRequest: 400,
  notFound: 404,
  unavailable: 503,
  contentType: 'application/json',
  post: 'POST',
  protocol: 'http:'
} as const;
