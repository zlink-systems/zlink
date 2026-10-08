import * as os from 'node:os';
import { PerfClock, ClockMetadata } from './clock';
import {
  DecimalText,
  NullReason,
  nullReason,
  PerfEchoRequest,
  PerfTriggerReply,
  PerfTriggerRequest,
  PerfValidationException,
  ResetReply,
  ResetRequest,
  RoleConfig
} from './contracts';
import { classifyError } from './errors';
import { Histogram } from './histogram';
import { MetricCatalog } from './metric-catalog';
import { PayloadPattern } from './payload';
import { ProcessSampler } from './process-sampler';

export type MeasurementConfig = Pick<
  RoleConfig,
  | 'runId'
  | 'cellId'
  | 'configHash'
  | 'language'
  | 'role'
  | 'roleInstance'
  | 'workload'
  | 'provenance'
>;

export interface PerfMetricsSnapshot {
  schemaVersion: number;
  runId: string;
  cellId: string;
  resetSeq: string;
  language: 'node';
  role: string;
  roleInstance: number;
  configHash: string;
  phase: string;
  window: {
    startedAtUnixMs: string | null;
    endedAtUnixMs: string | null;
    startTicks: string | null;
    endTicks: string | null;
    measuredSeconds: number | null;
  };
  clock: ClockMetadata;
  serializedMessageBytes: {
    direction: string;
    packetName: string;
    logicalPayloadBytes: string;
    observedSerializedBytes: string | null;
  }[];
  metrics: Record<string, unknown>;
  histograms: Record<string, unknown>;
  nullReasons: Record<string, NullReason>;
  publicStatus: unknown;
  publicMetrics: unknown[];
  runtimeMetrics: Record<string, unknown>;
  provenance: Record<string, unknown>;
}

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

// Application cohort accounting only. No socket state, retry, transport polling or completion pump.
// Node runs every callback on one event loop, so no counter needs a lock.
export class Measurement {
  private readonly sampler = new ProcessSampler();
  private latency = new Histogram();
  private counts = new Map<string, number>();
  private byKind = new Map<string, number>();
  private harness = new Map<string, number>();
  private language = new Map<string, number>();
  private errors: unknown[] = [];
  private readonly publicStateSamples: unknown[] = [];
  private directional = new Map<string, number>();
  private inflight = 0;
  private maxInflight = 0;
  private activeHandlers = 0;
  private drainWaiter: (() => void) | undefined;
  private start = 0n;
  private end = 0n;
  private startUnix: string | null = null;
  private endUnix: string | null = null;
  private currentPhase = 'setup';
  private currentResetSeq = '0';
  private sealedResults = false;
  private resetAck: ResetReply | undefined;
  private readonly starts = new Map<string, PerfTriggerReply>();
  private phaseDone: Promise<void> = Promise.resolve();
  private phaseFinished = true;
  readonly pattern: PayloadPattern;

  connected = 0;
  connectionFailures = 0;
  setupEvidence: unknown[] = [];
  preparationEvidence: Record<string, unknown> = {};
  samplePublicState: (() => unknown) | undefined;
  // A scenario's own counters: cleared with the window at reset, and added to every snapshot (family metrics, §14).
  onReset: (() => void) | undefined;
  enrichSnapshot: ((snapshot: PerfMetricsSnapshot) => void) | undefined;
  // True only while the runner reads the final snapshot that seals phase-owned artifacts.
  finalSnapshot = false;
  // The typed messages this scenario's measured path carries, one serializedMessageBytes row each (§15.2).
  messageTypes: { direction: string; packetName: string }[] = [
    { direction: 'request', packetName: 'PerfEchoRequest' },
    { direction: 'reply', packetName: 'PerfEchoReply' }
  ];

  constructor(
    readonly config: MeasurementConfig,
    private readonly primary: boolean
  ) {
    this.pattern = new PayloadPattern(config.workload.payloadSize);
  }

  get phase(): string {
    return this.currentPhase;
  }
  get resetSeq(): string {
    return this.currentResetSeq;
  }
  get startTicks(): bigint {
    return this.start;
  }
  get endTicks(): bigint {
    return this.end;
  }
  get phaseTask(): Promise<void> {
    return this.phaseDone;
  }
  get canIssue(): boolean {
    return !this.sealedResults && this.start !== 0n && PerfClock.now() < this.end;
  }
  get hasErrors(): boolean {
    return this.byKind.size + this.harness.size + this.language.size !== 0;
  }
  get errorEvidence(): unknown[] {
    return [...this.errors];
  }

  callDeadlineTicks(): bigint {
    return this.start === 0n
      ? PerfClock.now() + BigInt(this.config.workload.setupTimeoutMs) * 1_000_000n
      : this.end + BigInt(this.config.workload.drainTimeoutMs) * 1_000_000n;
  }

  callTimeout(driver = false): number {
    const remaining = this.callDeadlineTicks() - PerfClock.now();
    const timeoutMs = Number(remaining > 0n ? (remaining + 999_999n) / 1_000_000n : 1n);
    return timeoutMs + (driver ? this.config.workload.drainTimeoutMs : 0);
  }

  request(stream: number, sequence: number, probe = false): PerfEchoRequest {
    const warmup = probe || this.phase === 'warmup';
    const phase = warmup ? 'warmup' : 'measured';
    return new PerfEchoRequest({
      runId: this.config.runId,
      cellId: this.config.cellId,
      resetSeq: probe ? '0' : this.resetSeq,
      phase,
      clientId: stream,
      sequence: String(sequence),
      correlationId: `${this.config.cellId}/${phase}/${stream}/${sequence}`,
      sentTicks: PerfClock.now().toString(),
      clockDomainId: PerfClock.domain,
      returnSpotId: null,
      returnChannel: null,
      payload: this.pattern.base64
    });
  }

  // A send/send request names its return address (§10.4, §10.6); an echo request names none.
  validateRequest(
    request: PerfEchoRequest,
    returnChannel: string | null = null,
    returnSpotId: string | null = null
  ): void {
    if (
      request.runId !== this.config.runId ||
      request.cellId !== this.config.cellId ||
      !(request.clientId >= 0) ||
      (request.phase !== 'warmup' && request.phase !== 'measured') ||
      request.returnSpotId !== returnSpotId ||
      request.returnChannel !== returnChannel ||
      request.correlationId !==
        `${request.cellId}/${request.phase}/${request.clientId}/${request.sequence}` ||
      !request.clockDomainId
    )
      throw new PerfValidationException(
        'IdentityMismatch',
        'Request identity does not match the cell.'
      );
    DecimalText.u64(request.sequence);
    DecimalText.i64(request.sentTicks);
    const seq = DecimalText.u64(request.resetSeq);
    if (request.phase === 'warmup' ? seq !== 0n : seq === 0n || request.resetSeq !== this.resetSeq)
      throw new PerfValidationException(
        'PhaseMismatch',
        'Request reset sequence does not match the phase.'
      );
    this.pattern.validate(request.payload);
  }

  startPhase(
    trigger: PerfTriggerRequest,
    workload: (() => Promise<void>) | undefined
  ): PerfTriggerReply {
    const reject = (reason: string): PerfTriggerReply =>
      this.ack(trigger, false, 'rejected', reason);
    if (
      trigger.runId !== this.config.runId ||
      trigger.cellId !== this.config.cellId ||
      (trigger.phase !== 'warmup' && trigger.phase !== 'measured') ||
      trigger.resetSeq !== this.currentResetSeq ||
      (trigger.phase === 'warmup' ? this.currentResetSeq !== '0' : this.currentResetSeq === '0')
    )
      return reject('Identity, resetSeq or phase is invalid.');
    const key = `${trigger.phase}/${trigger.resetSeq}`;
    const previous = this.starts.get(key);
    if (previous) return { ...previous, state: 'alreadyStarted' };
    if (
      !this.phaseFinished ||
      !this.isDrained() ||
      (trigger.phase === 'warmup' ? this.currentPhase !== 'setup' : this.currentPhase !== 'reset')
    )
      return reject('Previous phase has not drained and reset.');
    this.currentPhase = trigger.phase;
    this.sealedResults = false;
    this.start = PerfClock.now();
    this.end =
      this.start +
      BigInt(
        Math.round(
          (trigger.phase === 'warmup'
            ? this.config.workload.warmupSeconds
            : this.config.workload.durationSeconds) * 1e9
        )
      );
    this.startUnix = PerfClock.unixMs();
    this.sampler.start();
    const reply = this.ack(trigger, true, 'started', null);
    this.starts.set(key, reply);
    // Running the phase on a later tick lets the HTTP/control acknowledgement leave before load starts.
    this.phaseFinished = false;
    this.phaseDone = new Promise<void>((resolve) => setImmediate(resolve)).then(() =>
      this.runPhase(workload)
    );
    return reply;
  }

  private ack(
    trigger: PerfTriggerRequest,
    accepted: boolean,
    state: PerfTriggerReply['state'],
    reason: string | null
  ): PerfTriggerReply {
    return {
      runId: trigger.runId,
      cellId: trigger.cellId,
      resetSeq: trigger.resetSeq,
      phase: trigger.phase,
      accepted,
      state,
      reason,
      configHash: this.config.configHash
    };
  }

  private async runPhase(workload: (() => Promise<void>) | undefined): Promise<void> {
    const phase = this.currentPhase;
    let operations: Promise<void>;
    try {
      operations = workload ? workload() : Promise.resolve();
    } catch (error) {
      this.recordDiagnostic(error);
      operations = Promise.resolve();
    }
    operations = operations.catch((error: unknown) => this.recordDiagnostic(error));
    const initial = this.samplePublicState?.();
    if (initial !== undefined) this.publicStateSamples.push(initial);
    for (;;) {
      const remaining = this.end - PerfClock.now();
      if (remaining <= 0n) break;
      await sleep(Math.min(100, Number(remaining) / 1e6));
      const publicState = this.samplePublicState?.();
      this.sampler.sample();
      if (publicState !== undefined) this.publicStateSamples.push(publicState);
    }
    this.sampler.end();
    this.endUnix = PerfClock.unixMs();
    if (phase === 'warmup') {
      await operations;
      await this.waitForDrain();
    }
    this.sealedResults = true;
    this.currentPhase = 'complete';
    this.phaseFinished = true;
  }

  private isDrained(): boolean {
    return this.inflight === 0 && this.activeHandlers === 0;
  }

  private waitForDrain(): Promise<void> {
    if (this.isDrained()) return Promise.resolve();
    return new Promise<void>((resolve) => {
      this.drainWaiter = resolve;
    });
  }

  private notifyDrained(): void {
    const resolve = this.drainWaiter;
    if (resolve === undefined || !this.isDrained()) return;
    this.drainWaiter = undefined;
    resolve();
  }

  resetPhase(request: ResetRequest, resetCapacity: (() => bigint) | undefined): ResetReply {
    const requested = DecimalText.u64(request.resetSeq);
    const drained =
      this.phaseFinished && (this.start === 0n || PerfClock.now() >= this.end) && this.isDrained();
    if (
      request.runId === this.config.runId &&
      request.cellId === this.config.cellId &&
      this.resetAck?.resetSeq === request.resetSeq &&
      drained
    )
      return this.resetAck;
    const reason =
      request.runId !== this.config.runId || request.cellId !== this.config.cellId
        ? 'Different run or cell.'
        : requested === 0n || requested <= DecimalText.u64(this.currentResetSeq)
          ? 'resetSeq must advance.'
          : !drained || this.currentPhase === 'setup'
            ? 'Warmup or measured operations have not drained.'
            : this.hasErrors
              ? 'Previous phase failed.'
              : null;
    if (reason !== null) {
      return {
        ok: false,
        runId: request.runId,
        cellId: request.cellId,
        role: this.config.role,
        roleInstance: this.config.roleInstance,
        resetSeq: request.resetSeq,
        applicationResetAtUnixMs: PerfClock.unixMs(),
        capacityEpoch: null,
        reason,
        nullReasons: {}
      };
    }
    this.counts.clear();
    this.byKind.clear();
    this.harness.clear();
    this.language.clear();
    this.errors = [];
    this.directional.clear();
    this.publicStateSamples.length = 0;
    this.latency = new Histogram();
    this.maxInflight = 0;
    this.start = this.end = 0n;
    this.startUnix = this.endUnix = null;
    this.sealedResults = false;
    this.onReset?.();
    this.currentResetSeq = request.resetSeq;
    this.currentPhase = 'reset';
    const resetAt = PerfClock.unixMs();
    const epoch = resetCapacity?.();
    const reasons: Record<string, NullReason> = {};
    if (epoch === undefined)
      reasons['/capacityEpoch'] = nullReason(
        'NOT_APPLICABLE',
        'The client owns no Framework host.'
      );
    this.resetAck = {
      ok: true,
      runId: this.config.runId,
      cellId: this.config.cellId,
      role: this.config.role,
      roleInstance: this.config.roleInstance,
      resetSeq: this.currentResetSeq,
      applicationResetAtUnixMs: resetAt,
      capacityEpoch: epoch === undefined ? null : epoch.toString(),
      reason: null,
      nullReasons: reasons
    };
    return this.resetAck;
  }

  beginOperation(direction = 'request'): bigint | undefined {
    const started = PerfClock.now();
    if (this.sealedResults || this.start === 0n || started >= this.end) return undefined;
    this.increment(this.counts, 'sent');
    this.inflight++;
    this.maxInflight = Math.max(this.maxInflight, this.inflight);
    this.increment(this.directional, direction);
    return started;
  }

  // completedTicks: a send/send echo keeps the time it was observed even when the first send's terminal comes later (§13).
  completeOperation(started: bigint, error?: unknown, completedTicks?: bigint): boolean {
    const completed = completedTicks ?? PerfClock.now();
    if (this.sealedResults) return false;
    this.inflight--;
    if (this.drainWaiter !== undefined) this.notifyDrained();
    if (completed >= this.end) return false;
    if (error !== undefined && error !== null) {
      this.recordError(error, true);
      return false;
    }
    this.increment(this.counts, 'completed');
    this.latency.record(completed - started);
    return true;
  }

  handlerEnter(): void {
    this.activeHandlers++;
  }
  handlerExit(): void {
    this.activeHandlers--;
    if (this.drainWaiter !== undefined) this.notifyDrained();
  }
  recordReply(request: PerfEchoRequest): void {
    this.recordApplicationCall(request, 'reply');
  }

  // A public call this process starts (send) or a typed reply it returns, counted once inside its own window.
  recordApplicationCall(request: { resetSeq: string; phase: string }, direction: string): void {
    if (
      request.resetSeq === this.currentResetSeq &&
      this.start !== 0n &&
      PerfClock.now() < this.end &&
      request.phase === (this.currentResetSeq === '0' ? 'warmup' : 'measured')
    )
      this.increment(this.directional, direction);
  }

  recordDiagnostic(error: unknown): void {
    if (!this.sealedResults) this.recordError(error, false);
  }

  private recordError(error: unknown, outcome: boolean): void {
    const classified = classifyError(error);
    this.increment(
      classified.namespace === 'byKind'
        ? this.byKind
        : classified.namespace === 'harness'
          ? this.harness
          : this.language,
      classified.key
    );
    if (outcome) this.increment(this.counts, classified.category);
    if (this.errors.length < 32) {
      const shaped = error as Error;
      this.errors.push({
        type: error instanceof Error ? error.constructor.name : typeof error,
        message: error instanceof Error ? shaped.message : String(error),
        publicKind: classified.publicKind,
        harnessKind: classified.harnessKind,
        connectorCode: classified.connectorCode,
        unrecognizedFrameworkKind: classified.unrecognizedFrameworkKind ?? null,
        stack: error instanceof Error ? (shaped.stack ?? '').split('\n').slice(0, 6) : null
      });
    }
  }

  private increment(values: Map<string, number>, key: string): void {
    values.set(key, (values.get(key) ?? 0) + 1);
  }
  private count(key: string): number {
    return this.counts.get(key) ?? 0;
  }
  private static text(values: Map<string, number>): Record<string, string> {
    return Object.fromEntries([...values].map(([key, value]) => [key, String(value)]));
  }

  snapshot(publicStatus: unknown): PerfMetricsSnapshot {
    const metrics: Record<string, unknown> = {};
    const histograms: Record<string, unknown> = {};
    const runtime: Record<string, unknown> = {};
    const reasons: Record<string, NullReason> = {};
    const workload = this.config.workload;
    MetricCatalog.baselineNulls(metrics, histograms, reasons);
    for (const key of MetricCatalog.outcomes) {
      if (this.primary) {
        const inflightAtEnd =
          this.count('sent') -
          this.count('completed') -
          this.count('failed') -
          this.count('timeout') -
          this.count('cancelled');
        metrics[`messages.${key}`] = String(
          key === 'inflightAtEnd' ? inflightAtEnd : this.count(key)
        );
      } else
        MetricCatalog.setNull(
          metrics,
          reasons,
          'metrics',
          `messages.${key}`,
          'NOT_APPLICABLE',
          'Echo outcomes belong to the source process.'
        );
    }
    if (this.primary) {
      this.latency.export('latencyMs', 'latency', metrics, histograms, reasons);
    } else {
      for (const suffix of ['meanMs', 'p50Ms', 'p95Ms', 'p99Ms', 'maxMs'])
        MetricCatalog.setNull(
          metrics,
          reasons,
          'metrics',
          `latency.${suffix}`,
          'NOT_APPLICABLE',
          'RTT belongs to the source process.'
        );
      MetricCatalog.setNull(
        histograms,
        reasons,
        'histograms',
        'latencyMs',
        'NOT_APPLICABLE',
        'RTT belongs to the source process.'
      );
    }
    const csClient = this.config.role === 'client' && workload.connections !== null;
    for (const key of ['requested', 'connected', 'failed']) {
      if (csClient) {
        const connections = workload.connections as number;
        metrics[`connections.${key}`] = String(
          key === 'requested'
            ? Math.floor(connections / workload.clientCount) +
                (this.config.roleInstance < connections % workload.clientCount ? 1 : 0)
            : key === 'connected'
              ? this.connected
              : this.connectionFailures
        );
      } else
        MetricCatalog.setNull(
          metrics,
          reasons,
          'metrics',
          `connections.${key}`,
          'NOT_APPLICABLE',
          'This process owns no physical connector pool.'
        );
    }
    for (const key of ['logicalStreams', 'inflight.max']) {
      if (this.primary && (key !== 'logicalStreams' || !csClient))
        metrics[`load.${key}`] = String(
          key === 'logicalStreams' ? (workload.logicalStreams ?? 0) : this.maxInflight
        );
      else
        MetricCatalog.setNull(
          metrics,
          reasons,
          'metrics',
          `load.${key}`,
          'NOT_APPLICABLE',
          'No server logical streams are owned here; CS slots are connector based.'
        );
    }
    for (const direction of ['request', 'send', 'reply', 'event']) {
      const count = this.directional.get(direction) ?? 0;
      metrics[`applicationMessages.${direction}`] = String(count);
      metrics[`applicationPayloadBytes.${direction}`] = String(count * workload.payloadSize);
    }
    const seconds = this.start === 0n ? null : Number(this.end - this.start) / 1e9;
    const applicationCount = [...this.directional.values()].reduce((a, b) => a + b, 0);
    metrics['throughput.kops'] =
      this.primary && seconds !== null && seconds > 0
        ? this.count('completed') / seconds / 1000
        : null;
    metrics['throughput.messagesPerSec'] =
      seconds !== null && seconds > 0 ? applicationCount / seconds : null;
    metrics['throughput.megabytesPerSec'] =
      seconds !== null && seconds > 0
        ? (applicationCount * workload.payloadSize) / seconds / 1048576
        : null;
    metrics['errors.byKind'] = Measurement.text(this.byKind);
    metrics['errors.harness'] = Measurement.text(this.harness);
    metrics['errors.language'] = Measurement.text(this.language);
    for (const key of [
      'throughput.kops',
      'throughput.messagesPerSec',
      'throughput.megabytesPerSec'
    ])
      if (metrics[key] === null)
        reasons[`/metrics/${key}`] = nullReason(
          this.start === 0n ? 'PHASE_NOT_STARTED' : 'NOT_APPLICABLE',
          'No applicable completed measurement window.'
        );
    if (this.currentPhase === 'complete') this.sampler.export(metrics, runtime, reasons);
    else {
      for (const key of [
        'process.cpuPercent',
        'process.rssMb',
        'process.allocatedMb',
        'gc.gen0',
        'gc.gen1',
        'gc.gen2'
      ])
        MetricCatalog.setNull(
          metrics,
          reasons,
          'metrics',
          key,
          'PHASE_NOT_STARTED',
          'Process window sampling has not completed.'
        );
    }
    runtime.setupEvidence = {
      name: 'setupEvidence',
      unit: 'observation',
      type: 'array',
      value: this.setupEvidence
    };
    runtime.preparationEvidence = {
      name: 'preparation evidence',
      unit: 'observation',
      type: 'object',
      value: this.preparationEvidence
    };
    runtime.publicReadinessSamples = {
      name: 'public host readiness and pressure samples',
      unit: 'observation',
      type: 'array',
      value: this.publicStateSamples
    };
    runtime.errors = {
      name: 'firstErrors',
      unit: 'observation',
      type: 'array',
      value: this.errors
    };
    runtime.activeHandlers = {
      name: 'application active handlers',
      unit: 'count',
      type: 'integer',
      value: String(this.activeHandlers)
    };
    const window = {
      startedAtUnixMs: this.startUnix,
      endedAtUnixMs: this.endUnix,
      startTicks: this.start === 0n ? null : this.start.toString(),
      endTicks: this.end === 0n ? null : this.end.toString(),
      measuredSeconds: seconds
    };
    for (const [key, value] of Object.entries(window))
      if (value === null)
        reasons[`/window/${key}`] = nullReason(
          'PHASE_NOT_STARTED',
          'Measurement window has not completed.'
        );
    for (const key of ['alignmentMethod', 'maxErrorNs', 'validFromTicks', 'validThroughTicks'])
      reasons[`/clock/${key}`] = nullReason(
        'NOT_APPLICABLE',
        'RTT uses the caller process clock only.'
      );
    if (publicStatus === null || publicStatus === undefined)
      reasons['/publicStatus'] = nullReason(
        'NOT_APPLICABLE',
        'The client has no Framework host runtime.'
      );
    const serialized = this.messageTypes.map((type) => ({
      direction: type.direction,
      packetName: type.packetName,
      logicalPayloadBytes: String(workload.payloadSize),
      observedSerializedBytes: null
    }));
    serialized.forEach((_row, index) => {
      reasons[`/serializedMessageBytes/${index}/observedSerializedBytes`] = nullReason(
        'PUBLIC_OBSERVATION_UNSUPPORTED',
        'No public per-DTO serialized byte observation; the measured message is serialized once by the Framework.'
      );
    });
    const provenance: Record<string, unknown> = {
      ...this.config.provenance,
      pid: process.pid,
      host: os.hostname(),
      messageCountScope: 'application-call-boundaries',
      configHash: this.config.configHash,
      resetAcknowledgement: this.resetAck ?? null,
      primaryEchoOwner: this.primary,
      runtimeVersion: process.version,
      effectiveProcessorCount: os.availableParallelism(),
      executor: {
        name: 'Node.js event loop (single JavaScript thread) with libuv thread pool',
        uvThreadpoolSize: process.env.UV_THREADPOOL_SIZE ?? '4 (libuv default)',
        v8: process.versions.v8,
        nodeVersion: process.version
      }
    };
    const snapshot: PerfMetricsSnapshot = {
      schemaVersion: 2,
      runId: this.config.runId,
      cellId: this.config.cellId,
      resetSeq: this.currentResetSeq,
      language: this.config.language,
      role: this.config.role,
      roleInstance: this.config.roleInstance,
      configHash: this.config.configHash,
      phase: this.currentPhase,
      window,
      clock: PerfClock.metadata(),
      serializedMessageBytes: serialized,
      metrics,
      histograms,
      nullReasons: reasons,
      publicStatus: publicStatus ?? null,
      publicMetrics: [],
      runtimeMetrics: runtime,
      provenance
    };
    this.enrichSnapshot?.(snapshot);
    return snapshot;
  }

  dispose(): void {
    this.sampler.dispose();
  }
}
