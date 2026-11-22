export const Names = {
  mesh: 'my-game-tables',
  stream: 'players',
  actor: 'Player',
  table: 'Table'
} as const;
// Words in server console lines that the run-server console also reads to show server titles.
export const ServerLog = {
  connectedTo: 'connected to',
  disconnectedFrom: 'disconnected from',
  // A routing ID is "<node id>-<suffix>" (setRoutingIdPrefix).
  routingIdSeparator: '-'
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
const startupMs = 30_000;
const messageMs = 10_000;
const shutdownMs = 10_000;
const safeMs = messageFollowMs + relocationMs;
export const Limits = {
  observationCapacity: 64,
  messageMs,
  startupMs,
  messageFollowMs,
  relocationMs,
  safeMs,
  shutdownMs,
  controlMs: Math.max(messageMs + relocationMs + safeMs, shutdownMs + messageMs + startupMs)
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
  packetNotAllowed: 'packet-not-allowed'
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
  actionRejected: 'ActionRejected'
} as const;

export const Control = {
  ready: 'READY ',
  drain: 'drain',
  replace: 'replace',
  shutdown: 'shutdown',
  requested: 'RELOCATION_REQUESTED',
  relocated: 'RELOCATED',
  safe: 'SAFE_TO_SHUTDOWN',
  stopped: 'STOPPED '
} as const;

export const NodeEntrypoints = {
  session: 'server/session/main.ts',
  table: 'server/table/main.ts'
} as const;
export type NodeRole = keyof typeof NodeEntrypoints;

export const LogFiles = {
  directory: 'logs',
  nodeExtension: '.log',
  runs: '.logs',
  runPrefix: 'run-',
  serverState: 'server.json',
  workerConfig: 'my-game.diagnostics',
  // Docker e2e nodes write their file log inside the container; the runner keeps their stderr.
  container: '/tmp/my-game-logs',
  // Console tag for lines from run-server itself.
  hostTag: 'host',
  // HH:MM:SS.mmm of an ISO timestamp, for console lines.
  consoleTimeStart: 11,
  consoleTimeEnd: 23
} as const;

const loopback = '127.0.0.1';
const redisPort = '6379';
export const LocalHost = {
  address: loopback,
  controlPort: 0,
  meshEndpoint: `tcp://${loopback}:0`,
  streamEndpoint: `ws://${loopback}:0`,
  redisPort,
  dockerRedis: `redis:${redisPort}`,
  keyPrefix: 'my-game:',
  projectPrefix: 'my-game-',
  structureFile: 'zlink.structure.json',
  ready: 'Server ready:',
  traceFlag: '--trace'
} as const;
