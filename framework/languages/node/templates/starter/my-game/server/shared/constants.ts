export const Names = { mesh: 'my-game-world', stream: 'players' } as const;
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
export const Scenario = { connect: 'connect' } as const;
export const Scenarios = Object.values(Scenario);
export const Control = {
  ready: 'READY ',
  drain: 'drain',
  shutdown: 'shutdown',
  requested: 'RELOCATION_REQUESTED',
  relocated: 'RELOCATED',
  safe: 'SAFE_TO_SHUTDOWN',
  stopped: 'STOPPED '
} as const;
export const NodeEntrypoints = { session: 'server/session/main.ts' } as const;
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

export const ServerLog = {
  connectedTo: 'connected to',
  disconnectedFrom: 'disconnected from',
  // A routing ID is "<node id>-<suffix>" (setRoutingIdPrefix).
  routingIdSeparator: '-'
} as const;
