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
  maxStep: 2,
  maxSpeed: 10,
  fireIntervalMs: 250,
  eyeHeight: 1.6,
  playerHalfWidth: 0.5,
  playerHeight: 2
} as const;
export const Rejection = {
  blockedByWall: 'blocked-by-wall',
  admissionRejected: 'admission-rejected',
  verticalMove: 'vertical-move',
  nonFinite: 'non-finite',
  stepTooLarge: 'step-too-large',
  outsideMap: 'outside-map',
  moveBlockedByWall: 'move-blocked-by-wall',
  moveBudget: 'move-budget',
  invalidAim: 'invalid-aim',
  fireTooSoon: 'fire-too-soon',
  staleSequence: 'stale-sequence',
  matchFinished: 'match-finished',
  packetNotAllowed: 'packet-not-allowed'
} as const;
export const ShotOutcome = {
  hit: 'hit',
  miss: 'miss',
  blockedByWall: Rejection.blockedByWall
} as const;
export enum MatchPhase {
  Waiting,
  Playing,
  Finished
}
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
export const Scenarios = ['matchmaking', 'hit', 'wall', 'input', 'reconnect', 'update'] as const;
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
