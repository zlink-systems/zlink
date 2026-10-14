export const Names = {
  mesh: 'my-game-tables',
  stream: 'players',
  actor: 'Player',
  table: 'Table',
} as const;
export const Game = {
  seats: 4,
  minimumPlayers: 2,
  initialChips: 1000,
  deckSize: 52,
  ranks: 13,
  lowestRank: 2,
  cardsPerPlayer: 2,
  noSeat: -1,
} as const;
export const Limits = {
  observationCapacity: 64,
  messageMs: 10_000,
  startupMs: 30_000,
  relocationMs: 25_000,
  shutdownMs: 10_000,
} as const;
export const Reasons = {
  full: 'table-full',
  noHand: 'no-hand',
  playing: 'hand-in-progress',
  players: 'not-enough-players',
  turn: 'not-your-turn',
  amount: 'invalid-amount',
  seated: 'not-seated',
} as const;
export const Scenarios = ['table', 'reconnect', 'update'] as const;
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
  deliverTableState: 'DeliverTableState',
} as const;

export const Control = {
  ready: 'READY ',
  drain: 'drain',
  shutdown: 'shutdown',
  requested: 'RELOCATION_REQUESTED',
  relocated: 'RELOCATED',
  safe: 'SAFE_TO_SHUTDOWN',
  stopped: 'STOPPED ',
} as const;
