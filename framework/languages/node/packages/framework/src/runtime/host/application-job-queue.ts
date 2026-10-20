import { SeverityNumber } from '@opentelemetry/api-logs';
import { telemetryLogger } from '../diagnostics/message-flow';
import { MILLISECONDS_PER_SECOND } from '../diagnostics/runtime-metrics';
import { ZLinkApplicationJobQueueProfile } from '../../contracts/Dispatch';
import {
  DEFAULT_PAUSE_THRESHOLD_PERCENT,
  DEFAULT_RESUME_THRESHOLD_PERCENT,
  APPLICATION_JOB_QUEUE_PERCENT_SCALE,
  isKnownApplicationJobQueueProfile,
  validateApplicationJobQueueMaximum,
  validateApplicationJobQueuePressureThresholds
} from '../../contracts/Configuration/Registration';
import { readFileSync } from 'node:fs';
import { availableParallelism } from 'node:os';
import type {
  ApplicationJobOrigin,
  ApplicationJobPermitPort,
  ApplicationJobReceiveRotationPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import {
  ApplicationJobReceiveFlowController,
  type ApplicationJobReceiveFlowTarget
} from '../application-jobs/receive-flow-controller';

export type ApplicationJobQueueProfileValue = 'compact' | 'low_latency' | 'balanced' | 'throughput';

export interface ApplicationJobQueueOptions {
  readonly profile?: ApplicationJobQueueProfileValue;
  readonly maxQueuedApplicationJobs?: bigint;
  readonly pauseThresholdPercent?: number;
  readonly resumeThresholdPercent?: number;
}

export interface ApplicationJobQueueConfiguration {
  readonly configuredProfile: ApplicationJobQueueProfileValue;
  readonly configuredManualMax?: bigint;
  readonly configuredPauseThresholdPercent: number;
  readonly configuredResumeThresholdPercent: number;
  readonly effectiveProcessorCount: bigint;
  readonly effectiveMaxQueuedApplicationJobs: bigint;
  readonly pausePermitCount: bigint;
  readonly resumePermitCount: bigint;
}

export type ApplicationJobQueuePressureState = 'running' | 'paused';

export interface ApplicationJobQueuePressureTransition {
  readonly state: ApplicationJobQueuePressureState;
  readonly sequence: bigint;
}

export interface ApplicationJobQueueSnapshot extends ApplicationJobQueueConfiguration {
  readonly reservedSupplyPermits: bigint;
  readonly queuedApplicationJobs: bigint;
  readonly permitsInUse: bigint;
  readonly peakPermitsInUse: bigint;
  readonly capacityWaiters: bigint;
  readonly capacityWaitCount: bigint;
  readonly capacityWaitDurationSeconds: number;
  readonly pressureState: ApplicationJobQueuePressureState;
  readonly currentPauseDurationSeconds: number;
  readonly runningTransitionCount: bigint;
  readonly pausedTransitionCount: bigint;
  readonly cumulativePauseDurationSeconds: number;
  readonly flowStateConfigFailureCount: bigint;
}

const CGROUP_V2_CPU_MAX = '/sys/fs/cgroup/cpu.max';
const CGROUP_V1_CPU_QUOTA = '/sys/fs/cgroup/cpu/cpu.cfs_quota_us';
const CGROUP_V1_CPU_PERIOD = '/sys/fs/cgroup/cpu/cpu.cfs_period_us';
const JOBS_PER_PROCESSOR: Readonly<Record<ApplicationJobQueueProfileValue, bigint>> = {
  compact: 32n,
  low_latency: 64n,
  balanced: 128n,
  throughput: 256n
};

export function resolveApplicationJobQueueConfiguration(
  options: ApplicationJobQueueOptions = {},
  effectiveProcessorCount: () => bigint = () => nodeEffectiveProcessorCount()
): ApplicationJobQueueConfiguration {
  const configuredProfile = options.profile ?? ZLinkApplicationJobQueueProfile.Balanced;
  if (!isKnownApplicationJobQueueProfile(configuredProfile)) {
    throw new TypeError('applicationJobQueueProfile must be a supported profile.');
  }

  const configuredManualMax = options.maxQueuedApplicationJobs;
  validateApplicationJobQueueMaximum(configuredManualMax, (message) => new RangeError(message));

  const processors = effectiveProcessorCount();
  if (typeof processors !== 'bigint' || processors < 1n) {
    throw new RangeError('effectiveProcessorCount must be a positive bigint.');
  }
  const effectiveMax = configuredManualMax ?? JOBS_PER_PROCESSOR[configuredProfile] * processors;
  validateApplicationJobQueueMaximum(
    effectiveMax,
    () => new RangeError('Application job queue profile calculation exceeds the supported range.')
  );

  const configuredPauseThresholdPercent =
    options.pauseThresholdPercent ?? DEFAULT_PAUSE_THRESHOLD_PERCENT;
  const configuredResumeThresholdPercent =
    options.resumeThresholdPercent ?? DEFAULT_RESUME_THRESHOLD_PERCENT;
  validateApplicationJobQueuePressureThresholds(
    configuredPauseThresholdPercent,
    configuredResumeThresholdPercent,
    (message) => new RangeError(message)
  );
  const percentScale = BigInt(APPLICATION_JOB_QUEUE_PERCENT_SCALE);
  const pauseNumerator = effectiveMax * BigInt(configuredPauseThresholdPercent);
  const resumeNumerator = effectiveMax * BigInt(configuredResumeThresholdPercent);

  return Object.freeze({
    configuredProfile,
    configuredManualMax,
    configuredPauseThresholdPercent,
    configuredResumeThresholdPercent,
    effectiveProcessorCount: processors,
    effectiveMaxQueuedApplicationJobs: effectiveMax,
    pausePermitCount: (pauseNumerator + percentScale - 1n) / percentScale,
    resumePermitCount: resumeNumerator / percentScale
  });
}

const enum QueuePermitCount {
  Reserved,
  Queued,
  Peak,
  Size
}

export class ApplicationJobQueuePermit implements ApplicationJobPermitPort {
  detachedForHandlerTurn = false;
  private state: 'idle' | 'reserved' | 'queued' | 'released';

  constructor(
    private readonly owner: ApplicationJobQueue,
    readonly origin: ApplicationJobOrigin,
    private readonly storage?: ApplicationJobReceiveRotation
  ) {
    this.state = storage === undefined ? 'reserved' : 'idle';
  }

  reserveStorage(): boolean {
    if (this.state !== 'idle') return false;
    this.state = 'reserved';
    this.detachedForHandlerTurn = false;
    return true;
  }

  reacquire(signal: AbortSignal): Promise<ApplicationJobPermitPort> {
    return this.owner.acquire(signal, this.origin);
  }

  releaseStorage(): void {
    if (this.storage === undefined || this.state === 'idle') return;
    if (this.state !== 'released')
      throw new Error('Reservation storage still owns queue capacity.');
    this.state = 'idle';
    this.storage.returnIdle(this);
  }

  detachForHandlerTurn(): void {
    this.detachedForHandlerTurn = true;
  }

  markApplicationQueued(): void {
    if (this.state !== 'reserved') {
      throw new Error('Application job queue permit is not reserved.');
    }
    this.owner.markQueued();
    this.state = 'queued';
  }

  releaseBeforeHandler(): void {
    if (this.owner.shouldHoldPermitBeforeHandler()) return;
    this.owner.releasePermit(this);
  }

  releaseAfterInternalProcessing(): void {
    this.owner.releasePermit(this);
  }

  releaseUnqueued(): void {
    if (this.state === 'reserved') this.owner.releasePermit(this);
  }

  takeReleaseState(): 'reserved' | 'queued' | undefined {
    if (this.state === 'released' || this.state === 'idle') return undefined;
    const state = this.state;
    this.state = 'released';
    return state;
  }
}

/**
 * Connection-owned storage; queue accounting remains with the host queue.
 * The rotation budget is a count reserved from the queue. Only a record that
 * is actually received takes a permit, from the idle storage stack, so a
 * rotation that receives one record touches one permit.
 */
class ApplicationJobReceiveRotation implements ApplicationJobReceiveRotationPort {
  private readonly idle: ApplicationJobQueuePermit[] = [];
  private granted = 0;
  private consumed = 0;

  constructor(
    private readonly owner: ApplicationJobQueue,
    private readonly capacity: number,
    private readonly origin: ApplicationJobOrigin
  ) {}

  reserve(): void {
    this.releaseUnused();
    this.granted = this.owner.tryReserveCount(this.capacity) ?? 0;
  }

  async acquire(signal: AbortSignal): Promise<void> {
    this.releaseUnused();
    this.granted = await new Promise<number>((resolve, reject) => {
      const count = this.owner.acquireCountOrResume(
        resolve,
        reject,
        signal,
        this.origin,
        this.capacity
      );
      if (count !== undefined) resolve(count);
    });
  }

  take(): ApplicationJobQueuePermit | undefined {
    if (this.consumed === this.granted) return undefined;
    this.consumed++;
    const permit = this.idle.pop() ?? new ApplicationJobQueuePermit(this.owner, this.origin, this);
    permit.reserveStorage();
    return permit;
  }

  releaseUnused(): void {
    // Unconsumed budget never became a permit; it returns as a count.
    this.owner.releaseUnusedReceiveRotation(this.granted - this.consumed);
    this.granted = 0;
    this.consumed = 0;
  }

  returnIdle(permit: ApplicationJobQueuePermit): void {
    this.idle.push(permit);
  }
}

interface CapacityWaiter {
  readonly origin: ApplicationJobOrigin;
  readonly requested: number;
  readonly resolve: (count: number) => void;
  readonly reject: (reason: unknown) => void;
  readonly signal?: AbortSignal;
  readonly abort: () => void;
  readonly startedAtMs: number;
  readonly metricsEpoch: bigint;
  active: boolean;
}

/**
 * Host-instance owner of the ordinary-ingress queue permit lifecycle.
 * It reserves capacity before receive/claim and transfers released capacity
 * directly to the oldest live waiter without allocating the configured limit.
 */
export class ApplicationJobQueue implements ApplicationJobQueuePort {
  private localWaiters = 0;
  private localBacklogLogged = false;
  // The contract bounds capacity by INT32_MAX; public snapshots remain bigint.
  private readonly permitCounts = new Uint32Array(QueuePermitCount.Size);
  private readonly limitCount: number;
  private readonly pauseCount: number;
  private readonly resumeCount: number;
  private capacityWaitCount = 0n;
  private capacityWaitDurationSeconds = 0;
  private metricsEpoch = 0n;
  private pressureStateValue: ApplicationJobQueuePressureState = 'running';
  private pressureTransitionSequence = 0n;
  private pauseStartedAtMs?: number;
  private cumulativePauseStartedAtMs?: number;
  private runningTransitionCount = 0n;
  private pausedTransitionCount = 0n;
  private cumulativePauseDurationSeconds = 0;
  private flowStateConfigFailureCount = 0n;
  private readonly pressureListeners = new Set<
    (state: ApplicationJobQueuePressureState, sequence: bigint) => void
  >();
  private readonly waiters: CapacityWaiter[] = [];
  private readonly receiveFlowTargets = new Map<
    object,
    ApplicationJobReceiveFlowTarget<ApplicationJobQueuePressureState>
  >();
  private readonly receiveFlowController: ApplicationJobReceiveFlowController<ApplicationJobQueuePressureState>;

  constructor(
    private readonly configuration: ApplicationJobQueueConfiguration,
    private readonly nowMs: () => number = () => performance.now(),
    private readonly handlerStartGate?: () => boolean,
    private readonly loggerFailureReporter?: (error: unknown) => void
  ) {
    this.limitCount = Number(configuration.effectiveMaxQueuedApplicationJobs);
    this.pauseCount = Number(configuration.pausePermitCount);
    this.resumeCount = Number(configuration.resumePermitCount);
    this.receiveFlowController = new ApplicationJobReceiveFlowController(this, (state) => state);
  }

  shouldHoldPermitBeforeHandler(): boolean {
    return this.handlerStartGate?.() === true;
  }

  createReceiveRotation(
    capacity: number,
    origin: ApplicationJobOrigin
  ): ApplicationJobReceiveRotation {
    if (!Number.isSafeInteger(capacity) || capacity < 1)
      throw new RangeError('Receive capacity must be positive.');
    return new ApplicationJobReceiveRotation(this, capacity, origin);
  }

  tryReserveCount(requested: number): number | undefined {
    if (this.waiters.length !== 0 || this.permitsInUse() === this.limitCount) return undefined;
    return this.reserveCount(requested);
  }

  registerReceiveFlowTarget(
    identity: object,
    applyState: (state: ApplicationJobQueuePressureState) => void,
    failureSink?: (error: unknown) => void
  ): boolean {
    this.unregisterReceiveFlowTarget(identity);
    const target: ApplicationJobReceiveFlowTarget<ApplicationJobQueuePressureState> = {
      setReceiveFlowState(state) {
        try {
          applyState(state);
        } catch (error) {
          try {
            failureSink?.(error);
          } catch {
            // Diagnostics cannot alter the absolute queue pressure state.
          }
          throw error;
        }
      }
    };
    this.receiveFlowTargets.set(identity, target);
    try {
      const registered = this.receiveFlowController.register(target);
      if (!registered && this.receiveFlowTargets.get(identity) === target) {
        this.receiveFlowTargets.delete(identity);
      }
      return registered;
    } catch (error) {
      if (this.receiveFlowTargets.get(identity) === target) {
        this.receiveFlowTargets.delete(identity);
      }
      throw error;
    }
  }

  unregisterReceiveFlowTarget(identity: object): void {
    const target = this.receiveFlowTargets.get(identity);
    if (target === undefined) return;
    this.receiveFlowTargets.delete(identity);
    this.receiveFlowController.unregister(target);
  }

  acquire(
    signal: AbortSignal | undefined,
    origin: ApplicationJobOrigin
  ): Promise<ApplicationJobQueuePermit> {
    return new Promise((resolve, reject) => {
      const permit = this.acquireOrResume(resolve, reject, signal, origin);
      if (permit !== undefined) resolve(permit);
    });
  }

  acquireOrResume(
    resume: (permit: ApplicationJobQueuePermit) => void,
    reject: (reason: unknown) => void,
    signal: AbortSignal | undefined,
    origin: ApplicationJobOrigin
  ): ApplicationJobQueuePermit | undefined {
    const count = this.acquireCountOrResume(
      () => resume(new ApplicationJobQueuePermit(this, origin)),
      reject,
      signal,
      origin,
      1
    );
    return count === undefined ? undefined : new ApplicationJobQueuePermit(this, origin);
  }

  acquireCountOrResume(
    resume: (count: number) => void,
    reject: (reason: unknown) => void,
    signal: AbortSignal | undefined,
    origin: ApplicationJobOrigin,
    requested: number
  ): number | undefined {
    if (signal?.aborted === true) {
      reject(abortReason(signal));
      return undefined;
    }
    const count = this.tryReserveCount(requested);
    if (count !== undefined) return count;

    new Promise<number>((resolve, reject) => {
      const waiter: CapacityWaiter = {
        origin,
        requested,
        resolve,
        reject,
        signal,
        abort: () => this.cancel(waiter),
        startedAtMs: this.nowMs(),
        metricsEpoch: this.metricsEpoch,
        active: true
      };
      this.waiters.push(waiter);
      this.capacityWaitCount += 1n;
      const warningCount = this.updateLocalWaiters(origin, 1);
      signal?.addEventListener('abort', waiter.abort, { once: true });
      this.drainWaiters();
      if (warningCount !== undefined) this.logLocalBacklog(warningCount);
    })
      .then(resume)
      .catch(reject);
    return undefined;
  }

  snapshot(): ApplicationJobQueueSnapshot {
    const permitsInUse = this.permitsInUse();
    const now = this.nowMs();
    return Object.freeze({
      ...this.configuration,
      reservedSupplyPermits: BigInt(this.permitCounts[QueuePermitCount.Reserved]!),
      queuedApplicationJobs: BigInt(this.permitCounts[QueuePermitCount.Queued]!),
      permitsInUse: BigInt(permitsInUse),
      peakPermitsInUse: BigInt(this.permitCounts[QueuePermitCount.Peak]!),
      capacityWaiters: BigInt(this.waiters.length),
      capacityWaitCount: this.capacityWaitCount,
      capacityWaitDurationSeconds: this.capacityWaitDurationSeconds,
      pressureState: this.pressureStateValue,
      currentPauseDurationSeconds: this.pauseDurationAt(now),
      runningTransitionCount: this.runningTransitionCount,
      pausedTransitionCount: this.pausedTransitionCount,
      cumulativePauseDurationSeconds:
        this.cumulativePauseDurationSeconds + this.cumulativePauseDurationAt(now),
      flowStateConfigFailureCount: this.flowStateConfigFailureCount
    });
  }

  resetMetrics(): void {
    this.metricsEpoch += 1n;
    this.permitCounts[QueuePermitCount.Peak] = this.permitsInUse();
    this.capacityWaitCount = 0n;
    this.capacityWaitDurationSeconds = 0;
    this.runningTransitionCount = 0n;
    this.pausedTransitionCount = 0n;
    this.cumulativePauseDurationSeconds = 0;
    this.cumulativePauseStartedAtMs =
      this.pressureStateValue === 'paused' ? this.nowMs() : undefined;
    this.flowStateConfigFailureCount = 0n;
  }

  pressureState(): ApplicationJobQueuePressureState {
    return this.pressureStateValue;
  }

  pressureTransition(): ApplicationJobQueuePressureTransition {
    return {
      state: this.pressureStateValue,
      sequence: this.pressureTransitionSequence
    };
  }

  onPressureStateChange(
    listener: (state: ApplicationJobQueuePressureState, sequence: bigint) => void
  ): () => void {
    this.pressureListeners.add(listener);
    return () => this.pressureListeners.delete(listener);
  }

  recordFlowStateConfigFailure(): void {
    this.flowStateConfigFailureCount += 1n;
  }

  markQueued(): void {
    if (this.permitCounts[QueuePermitCount.Reserved] === 0) {
      throw new Error('Application job queue has no reserved supply permit.');
    }
    this.permitCounts[QueuePermitCount.Reserved] =
      this.permitCounts[QueuePermitCount.Reserved]! - 1;
    this.permitCounts[QueuePermitCount.Queued] = this.permitCounts[QueuePermitCount.Queued]! + 1;
  }

  releasePermit(permit: ApplicationJobQueuePermit): void {
    const state = permit.takeReleaseState();
    if (state !== undefined) this.release(state);
  }

  releaseUnusedReceiveRotation(count: number): void {
    // Unconsumed budget still belongs to this receive round; no record has acquired it.
    if (count === 0) return;
    this.release('reserved', count);
  }

  release(state: 'reserved' | 'queued', count = 1): void {
    const index = state === 'reserved' ? QueuePermitCount.Reserved : QueuePermitCount.Queued;
    const available = this.permitCounts[index]!;
    if (available < count) {
      throw new Error(`Application job queue ${state} permit accounting underflow.`);
    }
    this.permitCounts[index] = available - count;
    this.drainWaiters();
    this.evaluatePressure();
  }

  private limit(): bigint {
    return this.configuration.effectiveMaxQueuedApplicationJobs;
  }

  private permitsInUse(): number {
    return (
      this.permitCounts[QueuePermitCount.Reserved]! + this.permitCounts[QueuePermitCount.Queued]!
    );
  }

  private reserveSupply(count: number, evaluatePressure = true): void {
    this.permitCounts[QueuePermitCount.Reserved] =
      this.permitCounts[QueuePermitCount.Reserved]! + count;
    this.permitCounts[QueuePermitCount.Peak] = Math.max(
      this.permitCounts[QueuePermitCount.Peak]!,
      this.permitsInUse()
    );
    if (evaluatePressure) this.evaluatePressure();
  }

  private reserveCount(requested: number, evaluatePressure = true): number {
    const count = Math.min(requested, this.limitCount - this.permitsInUse());
    this.reserveSupply(count, evaluatePressure);
    return count;
  }

  private drainWaiters(): void {
    while (this.waiters.length > 0 && this.permitsInUse() < this.limitCount) {
      const waiter = this.waiters.shift()!;
      if (!waiter.active) continue;
      waiter.active = false;
      waiter.signal?.removeEventListener('abort', waiter.abort);
      this.recordCompletedWait(waiter);
      waiter.resolve(this.reserveCount(waiter.requested, false));
    }
  }

  private cancel(waiter: CapacityWaiter): void {
    if (!waiter.active) return;
    waiter.active = false;
    const index = this.waiters.indexOf(waiter);
    if (index >= 0) this.waiters.splice(index, 1);
    waiter.signal?.removeEventListener('abort', waiter.abort);
    this.recordCompletedWait(waiter);
    waiter.reject(waiter.signal === undefined ? abortError() : abortReason(waiter.signal));
    this.drainWaiters();
  }

  private recordCompletedWait(waiter: CapacityWaiter): void {
    this.updateLocalWaiters(waiter.origin, -1);
    if (waiter.metricsEpoch !== this.metricsEpoch) return;
    const durationMs = Math.max(0, this.nowMs() - waiter.startedAtMs);
    this.capacityWaitDurationSeconds += durationMs / MILLISECONDS_PER_SECOND;
  }

  private updateLocalWaiters(origin: ApplicationJobOrigin, delta: number): number | undefined {
    if (origin !== 'local') return undefined;
    this.localWaiters += delta;
    if (this.localWaiters === 0) this.localBacklogLogged = false;
    if (!this.localBacklogLogged && this.localWaiters > Number(this.limit())) {
      this.localBacklogLogged = true;
      return this.localWaiters;
    }
    return undefined;
  }

  private logLocalBacklog(localWaiters: number): void {
    try {
      telemetryLogger.emit({
        eventName: 'zlink.runtime.host.local_job_backlog_exceeded',
        timestamp: new Date(),
        severityNumber: SeverityNumber.WARN,
        severityText: 'WARN',
        body: 'Same-host jobs waiting for a host permit exceeded the application job queue maximum.',
        attributes: {
          source_kind: 'host',
          source_name: 'zlink.runtime.host',
          local_waiters: localWaiters,
          effective_maximum: Number(this.limit())
        }
      });
    } catch (error) {
      this.loggerFailureReporter?.(error);
    }
  }

  private evaluatePressure(): void {
    const next =
      this.pressureStateValue === 'running'
        ? this.permitsInUse() >= this.pauseCount
          ? 'paused'
          : 'running'
        : this.permitsInUse() <= this.resumeCount
          ? 'running'
          : 'paused';
    if (next === this.pressureStateValue) return;

    const now = this.nowMs();
    if (next === 'paused') {
      this.pausedTransitionCount += 1n;
      this.pauseStartedAtMs = now;
      this.cumulativePauseStartedAtMs = now;
    } else {
      this.runningTransitionCount += 1n;
      this.cumulativePauseDurationSeconds += this.cumulativePauseDurationAt(now);
      this.pauseStartedAtMs = undefined;
      this.cumulativePauseStartedAtMs = undefined;
    }
    this.pressureStateValue = next;
    this.pressureTransitionSequence += 1n;
    const sequence = this.pressureTransitionSequence;
    for (const listener of this.pressureListeners) listener(next, sequence);
  }

  private pauseDurationAt(nowMs: number): number {
    if (this.pauseStartedAtMs === undefined) return 0;
    return Math.max(0, nowMs - this.pauseStartedAtMs) / MILLISECONDS_PER_SECOND;
  }

  private cumulativePauseDurationAt(nowMs: number): number {
    if (this.cumulativePauseStartedAtMs === undefined) return 0;
    return Math.max(0, nowMs - this.cumulativePauseStartedAtMs) / MILLISECONDS_PER_SECOND;
  }
}

export function nodeEffectiveProcessorCount(
  explicitExecutorMaximum?: number,
  runtimeConstrainedLogicalCount: number = availableParallelism(),
  readSystemText: (path: string) => string | undefined = readSystemFile
): bigint {
  const candidates = [
    positiveSafeInteger(runtimeConstrainedLogicalCount),
    cgroupQuotaProcessorCount(readSystemText),
    positiveSafeInteger(explicitExecutorMaximum)
  ].filter((candidate): candidate is number => candidate !== undefined);
  return BigInt(candidates.length === 0 ? 1 : Math.min(...candidates));
}

function cgroupQuotaProcessorCount(
  readSystemText: (path: string) => string | undefined
): number | undefined {
  const cpuMax = readSystemText(CGROUP_V2_CPU_MAX)?.trim().split(/\s+/u);
  if (cpuMax !== undefined && cpuMax.length >= 2 && cpuMax[0] !== 'max') {
    const count = quotaProcessorCount(cpuMax[0]!, cpuMax[1]!);
    if (count !== undefined) return count;
  }
  const quota = readSystemText(CGROUP_V1_CPU_QUOTA)?.trim();
  const period = readSystemText(CGROUP_V1_CPU_PERIOD)?.trim();
  return quota === undefined || period === undefined
    ? undefined
    : quotaProcessorCount(quota, period);
}

function quotaProcessorCount(quotaText: string, periodText: string): number | undefined {
  try {
    const quota = BigInt(quotaText);
    const period = BigInt(periodText);
    if (quota <= 0n || period <= 0n) return undefined;
    const floored = quota / period;
    const count = floored < 1n ? 1n : floored;
    return count > BigInt(Number.MAX_SAFE_INTEGER) ? Number.MAX_SAFE_INTEGER : Number(count);
  } catch {
    return undefined;
  }
}

function positiveSafeInteger(value: number | undefined): number | undefined {
  return value !== undefined && Number.isSafeInteger(value) && value > 0 ? value : undefined;
}

function readSystemFile(path: string): string | undefined {
  try {
    return readFileSync(path, 'utf8');
  } catch {
    return undefined;
  }
}

function abortReason(signal: AbortSignal): unknown {
  return signal.reason ?? abortError();
}

function abortError(): Error {
  const error = new Error('Application job queue capacity wait was aborted.');
  error.name = 'AbortError';
  return error;
}
