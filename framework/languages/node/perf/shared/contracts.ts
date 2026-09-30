// Perf spec §12/§15.2. Framework messages are classes because the Framework takes the packet name from the
// constructor name; the 64-bit fields are decimal strings (application fields, not a message codec).
import * as fs from 'node:fs';

export interface Identity {
  runId: string;
  cellId: string;
  resetSeq: string;
  phase: string;
}

export class PerfEchoRequest implements Identity {
  runId!: string;
  cellId!: string;
  resetSeq!: string;
  phase!: string;
  clientId!: number;
  sequence!: string;
  correlationId!: string;
  sentTicks!: string;
  clockDomainId!: string;
  returnSpotId!: string | null;
  returnChannel!: string | null;
  payload!: string;
  constructor(init: PerfEchoRequest | Record<string, unknown>) {
    Object.assign(this, init);
  }
  with(changes: Partial<PerfEchoRequest>): PerfEchoRequest {
    return new PerfEchoRequest({ ...this, ...changes });
  }
}

export class PerfEchoReply implements Identity {
  runId!: string;
  cellId!: string;
  resetSeq!: string;
  phase!: string;
  clientId!: number;
  sequence!: string;
  correlationId!: string;
  receivedTicks!: string;
  clockDomainId!: string;
  payload!: string;
  constructor(init: PerfEchoReply | Record<string, unknown>) {
    Object.assign(this, init);
  }
}

export class PerfDriveRequest {
  constructor(readonly echo: PerfEchoRequest) {}
}

export class PerfDriveReply {
  constructor(
    readonly started: boolean,
    readonly echo: PerfEchoReply | null
  ) {}
}

export interface PerfTriggerRequest extends Identity {}

export interface PerfTriggerReply extends Identity {
  accepted: boolean;
  state: 'started' | 'alreadyStarted' | 'rejected';
  configHash: string;
  reason: string | null;
}

export class PerfPublishEvent implements Identity {
  runId!: string;
  cellId!: string;
  resetSeq!: string;
  phase!: string;
  sequence!: string;
  topic!: string;
  sentTicks!: string;
  clockDomainId!: string;
  payload!: string;
  constructor(init: PerfPublishEvent | Record<string, unknown>) {
    Object.assign(this, init);
  }
}

export interface WorkerObservation {
  startedTicks: string;
  endedTicks: string;
  clockDomainId: string;
  iterations: string;
  checksum: number;
}

export interface ResetRequest {
  runId: string;
  cellId: string;
  resetSeq: string;
}

export interface NullReason {
  code: string;
  reason: string;
  owner: string;
  lowerBoundMs: number | null;
}

export function nullReason(code: string, reason: string, owner = 'perf/README.ko.md', lowerBoundMs: number | null = null): NullReason {
  return { code, reason, owner, lowerBoundMs };
}

export interface ResetReply {
  ok: boolean;
  runId: string;
  cellId: string;
  role: string;
  roleInstance: number;
  resetSeq: string;
  applicationResetAtUnixMs: string;
  capacityEpoch: string | null;
  reason: string | null;
  nullReasons: Record<string, NullReason>;
}

export interface PerfReady {
  runId: string;
  cellId: string;
  role: string;
  roleInstance: number;
  infrastructureReady: boolean;
  objectsReady: boolean;
  consumersReady: boolean;
  ready: boolean;
  observedAtUnixMs: string;
  evidence: unknown[];
  reasons: string[];
}

export interface Workload {
  payloadSize: number;
  durationSeconds: number;
  warmupSeconds: number;
  inflight: number;
  connections: number | null;
  logicalStreams: number | null;
  clientCount: number;
  connectConcurrency: number | null;
  requestTimeoutMs: number;
  correlationExpiryMs: number;
  settleTimeoutMs: number;
  setupTimeoutMs: number;
  adminTimeoutMs: number;
  socketSendTimeoutMs: number;
}

export interface StoreConfig {
  provider: string;
  endpoint: string;
  containerId: string;
  image: string;
  imageDigest: string;
  namespace: string;
}

export interface WorkerConfig {
  algorithm: string;
  taskMillis: number;
  minThreads: number;
  maxThreads: number;
  maxQueueLength: number;
  idleTimeoutMs: number;
  workerTimeoutMs: number;
}

export interface DiagnosticsConfig {
  level: string;
  flowFile: string;
}

// The role config file of §5.1 as the common runner writes it (framework/perf/runner/roles.py).
export interface RoleConfig {
  runId: string;
  cellId: string;
  configHash: string;
  role: string;
  roleInstance: number;
  scenario: string;
  mode: string;
  terminal: string;
  topology: string | null;
  channelName: string | null;
  meshName: string | null;
  transportEndpoints: Record<string, string>;
  peerEndpoint: string | null;
  metricsUrl: string;
  applicationTriggerUrl: string;
  source: boolean;
  objectRole: string;
  awaitRemoteTargets: boolean;
  store: StoreConfig | null;
  spotIds: string[];
  actorIds: string[];
  spotCount: number | null;
  subscriberCount: number | null;
  worker: WorkerConfig | null;
  executionMode: string;
  workload: Workload;
  diagnostics: DiagnosticsConfig | null;
  provenance: Record<string, unknown>;
}

export interface EndpointRole {
  role: string;
  roleInstance: number;
  configFile: string;
  streamEndpoint: string | null;
  applicationTriggerUrl: string;
  metrics: { transport: string; baseUrl: string };
  transportEndpoints: Record<string, string>;
  spotIds: string[];
  actorIds: string[];
}

export interface EndpointManifest {
  runId: string;
  cellId: string;
  configHash: string;
  workload: Workload;
  roles: EndpointRole[];
  provenance: Record<string, unknown>;
}

export function readJson<T>(file: string): T {
  return JSON.parse(fs.readFileSync(file, 'utf8')) as T;
}

// Harness failures (§14.2): application validation, correlation and phase errors; never a Framework kind.
export class PerfValidationException extends Error {
  constructor(
    readonly kind: string,
    message: string
  ) {
    super(message);
    this.name = 'PerfValidationException';
  }
}

// U64/I64 canonical decimal text (§15.2). Counts stay JS numbers (exact below 2^53); ticks are bigint.
export const DecimalText = {
  of: (value: number | bigint): string => value.toString(),
  u64(text: unknown): bigint {
    if (typeof text !== 'string' || !/^(0|[1-9][0-9]*)$/.test(text)) throw new PerfValidationException('SchemaMismatch', 'Noncanonical U64 decimal string.');
    const value = BigInt(text);
    if (value > 18446744073709551615n) throw new PerfValidationException('SchemaMismatch', 'U64 overflow.');
    return value;
  },
  i64(text: unknown): bigint {
    if (typeof text !== 'string' || !/^(0|-?[1-9][0-9]*)$/.test(text)) throw new PerfValidationException('SchemaMismatch', 'Noncanonical I64 decimal string.');
    const value = BigInt(text);
    if (value > 9223372036854775807n || value < -9223372036854775808n) throw new PerfValidationException('SchemaMismatch', 'I64 overflow.');
    return value;
  }
};

// Application admin/config/result JSON: bigint and Date are written as the §15.2 text, never a JSON number.
export function toJson(value: unknown): string {
  return JSON.stringify(value, (_key, item: unknown) => (typeof item === 'bigint' ? item.toString() : item));
}
