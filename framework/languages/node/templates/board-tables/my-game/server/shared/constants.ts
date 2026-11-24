export const Names = {
  mesh: 'my-game-tables',
  playerRecords: 'my-game-player-records',
  stream: 'players',
  actor: 'Player',
  table: 'Table',
  pool: 'Pool',
  poolKey: 'pool-board',
  matchRoomPrefix: 'room-'
} as const;
export const Game = {
  seats: 4,
  minimumPlayers: 2,
  initialChips: 1000n,
  deckSize: 52,
  ranks: 13,
  lowestRank: 2,
  cardsPerPlayer: 2,
  noSeat: -1
} as const;
const messageFollowMs = 30_000;
const relocationMs = 25_000;
export const Limits = {
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  messageFollowMs,
  relocationMs,
  safeMs: messageFollowMs + relocationMs,
  shutdownMs: 10_000
} as const;
export const Reasons = {
  full: 'table-full',
  noHand: 'no-hand',
  playing: 'hand-in-progress',
  players: 'not-enough-players',
  turn: 'not-your-turn',
  amount: 'invalid-amount',
  seated: 'not-seated',
  integerOutOfRange: 'integer-out-of-range',
  packetNotAllowed: 'packet-not-allowed',
  matchPending: 'match-pending',
  matchNotWaiting: 'match-not-waiting',
  matchRoomActive: 'match-room-active'
} as const;
export const MatchCancellation = {
  cancelled: 'cancelled',
  disconnected: 'disconnected',
  expired: 'expired'
} as const;
export const Matchmaking = {
  tickMs: 500,
  entryMs: 10_000,
  disconnectGraceMs: 15_000,
  waitMs: 120_000
} as const;
export const Scenarios = [
  'table',
  'reconnect',
  'match',
  'match-cancel',
  'update',
  'persist'
] as const;
export const PacketNames = {
  sessionInfoRequest: 'SessionInfoRequest',
  sessionInfo: 'SessionInfo',
  joinTableRequest: 'JoinTableRequest',
  tableViewRequest: 'TableViewRequest',
  startHandRequest: 'StartHandRequest',
  betRequest: 'BetRequest',
  tableView: 'TableView',
  handDealt: 'HandDealt',
  handResult: 'HandResult',
  actionRejected: 'ActionRejected',
  findMatch: 'FindMatch',
  matchQueued: 'MatchQueued',
  cancelMatch: 'CancelMatch',
  matchCancelled: 'MatchCancelled',
  matchFound: 'MatchFound',
  matchCommitted: 'MatchCommitted',
  matchRequest: 'MatchRequest',
  cancelTicket: 'CancelTicket',
  matchPresence: 'MatchPresence',
  matchAssigned: 'MatchAssigned',
  enterReport: 'EnterReport',
  prepareRoom: 'PrepareRoom',
  roomPrepared: 'RoomPrepared',
  roomClosed: 'RoomClosed',
  roomAdmission: 'RoomAdmission',
  tableAdmission: 'TableAdmission',
  commitRoom: 'CommitRoom',
  withdrawRoom: 'WithdrawRoom',
  matchReleased: 'MatchReleased',
  returnReport: 'ReturnReport',
  roomWithdrawn: 'RoomWithdrawn',
  loadPlayer: 'LoadPlayer',
  commitRound: 'CommitRound'
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
  table: 'server/table/main.ts',
  matchmaker: 'server/matchmaker/main.ts',
  api: 'server/api/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;
