import { readFileSync } from 'node:fs';
import { availableParallelism } from 'node:os';
import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import {
  ApplicationJobReceiveFlowController,
  type ApplicationJobReceiveFlowTarget
} from '../application-jobs/receive-flow-controller';

export type ApplicationJobQueueProfileValue =
  | 'compact'
  | 'low_latency'
  | 'balanced'
  | 'throughput';

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

const MAX_QUEUED_APPLICATION_JOBS = 2_147_483_647n;
const DEFAULT_PAUSE_THRESHOLD_PERCENT = 80;
const DEFAULT_RESUME_THRESHOLD_PERCENT = 60;
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
  const configuredProfile = options.profile ?? 'balanced';
  if (!(configuredProfile in JOBS_PER_PROCESSOR)) {
    throw new TypeError('applicationJobQueueProfile must be a supported profile.');
  }

  const configuredManualMax = options.maxQueuedApplicationJobs;
  if (configuredManualMax !== undefined
      && (typeof configuredManualMax !== 'bigint'
        || configuredManualMax < 1n
        || configuredManualMax > MAX_QUEUED_APPLICATION_JOBS)) {
    throw new RangeError(
      'maxQueuedApplicationJobs must be a bigint in the range 1..2147483647.'
    );
  }

  const processors = effectiveProcessorCount();
  if (typeof processors !== 'bigint' || processors < 1n) {
    throw new RangeError('effectiveProcessorCount must be a positive bigint.');
  }
  const effectiveMax = configuredManualMax
    ?? JOBS_PER_PROCESSOR[configuredProfile] * processors;
  if (effectiveMax < 1n || effectiveMax > MAX_QUEUED_APPLICATION_JOBS) {
    throw new RangeError(
      'Application job queue profile calculation exceeds the supported range.'
    );
  }

  const configuredPauseThresholdPercent = options.pauseThresholdPercent
    ?? DEFAULT_PAUSE_THRESHOLD_PERCENT;
  const configuredResumeThresholdPercent = options.resumeThresholdPercent
    ?? DEFAULT_RESUME_THRESHOLD_PERCENT;
  validatePressureThresholds(
    configuredPauseThresholdPercent,
    configuredResumeThresholdPercent
  );
  const pauseNumerator = effectiveMax * BigInt(configuredPauseThresholdPercent);
  const resumeNumerator = effectiveMax * BigInt(configuredResumeThresholdPercent);

  return Object.freeze({
    configuredProfile,
    configuredManualMax,
    configuredPauseThresholdPercent,
    configuredResumeThresholdPercent,
    effectiveProcessorCount: processors,
    effectiveMaxQueuedApplicationJobs: effectiveMax,
    pausePermitCount: (pauseNumerator + 99n) / 100n,
    resumePermitCount: resumeNumerator / 100n
  });
}

export class ApplicationJobQueuePermit implements ApplicationJobPermitPort {
  private state: 'reserved' | 'queued' | 'released' = 'reserved';

  constructor(private readonly owner: ApplicationJobQueue) {}

  markApplicationQueued(): void {
    if (this.state !== 'reserved') {
      throw new Error('Application job queue permit is not reserved.');
    }
    this.owner.markQueued();
    this.state = 'queued';
  }

  releaseBeforeHandler(): void {
    if (this.owner.shouldHoldPermitBeforeHandler()) return;
    this.release();
  }

  releaseAfterInternalProcessing(): void {
    this.release();
  }

  private release(): void {
    if (this.state === 'released') return;
    this.owner.release(this.state);
    this.state = 'released';
  }
}

interface CapacityWaiter {
  readonly resolve: (permit: ApplicationJobQueuePermit) => void;
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
  private reservedSupplyPermits = 0n;
  private queuedApplicationJobs = 0n;
  private peakPermitsInUse = 0n;
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
  private readonly pressureListeners = new Set<(
    state: ApplicationJobQueuePressureState,
    sequence: bigint
  ) => void>();
  private readonly waiters: CapacityWaiter[] = [];
  private readonly receiveFlowTargets = new Map<
    object,
    ApplicationJobReceiveFlowTarget<ApplicationJobQueuePressureState>
  >();
  private readonly receiveFlowController: ApplicationJobReceiveFlowController<
    ApplicationJobQueuePressureState
  >;

  constructor(
    private readonly configuration: ApplicationJobQueueConfiguration,
    private readonly nowMs: () => number = () => performance.now(),
    private readonly handlerStartGate?: () => boolean
  ) {
    this.receiveFlowController = new ApplicationJobReceiveFlowController(
      this,
      state => state
    );
  }

  shouldHoldPermitBeforeHandler(): boolean {
    return this.handlerStartGate?.() === true;
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

  acquire(signal?: AbortSignal): Promise<ApplicationJobQueuePermit> {
    if (signal?.aborted === true) {
      return Promise.reject(abortReason(signal));
    }
    if (this.waiters.length === 0 && this.permitsInUse() < this.limit()) {
      return Promise.resolve(this.reserve());
    }

    return new Promise<ApplicationJobQueuePermit>((resolve, reject) => {
      const waiter: CapacityWaiter = {
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
      signal?.addEventListener('abort', waiter.abort, { once: true });
      this.drainWaiters();
    });
  }

  snapshot(): ApplicationJobQueueSnapshot {
    const permitsInUse = this.permitsInUse();
    const now = this.nowMs();
    return Object.freeze({
      ...this.configuration,
      reservedSupplyPermits: this.reservedSupplyPermits,
      queuedApplicationJobs: this.queuedApplicationJobs,
      permitsInUse,
      peakPermitsInUse: this.peakPermitsInUse,
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
    this.peakPermitsInUse = this.permitsInUse();
    this.capacityWaitCount = 0n;
    this.capacityWaitDurationSeconds = 0;
    this.runningTransitionCount = 0n;
    this.pausedTransitionCount = 0n;
    this.cumulativePauseDurationSeconds = 0;
    this.cumulativePauseStartedAtMs = this.pressureStateValue === 'paused'
      ? this.nowMs()
      : undefined;
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
    if (this.reservedSupplyPermits === 0n) {
      throw new Error('Application job queue has no reserved supply permit.');
    }
    this.reservedSupplyPermits -= 1n;
    this.queuedApplicationJobs += 1n;
  }

  release(state: 'reserved' | 'queued'): void {
    if (state === 'reserved') {
      if (this.reservedSupplyPermits === 0n) {
        throw new Error('Application job queue reserved permit accounting underflow.');
      }
      this.reservedSupplyPermits -= 1n;
    } else {
      if (this.queuedApplicationJobs === 0n) {
        throw new Error('Application job queue queued permit accounting underflow.');
      }
      this.queuedApplicationJobs -= 1n;
    }
    this.drainWaiters();
    this.evaluatePressure();
  }

  private limit(): bigint {
    return this.configuration.effectiveMaxQueuedApplicationJobs;
  }

  private permitsInUse(): bigint {
    return this.reservedSupplyPermits + this.queuedApplicationJobs;
  }

  private reserve(evaluatePressure = true): ApplicationJobQueuePermit {
    this.reservedSupplyPermits += 1n;
    const current = this.permitsInUse();
    if (current > this.peakPermitsInUse) this.peakPermitsInUse = current;
    if (evaluatePressure) this.evaluatePressure();
    return new ApplicationJobQueuePermit(this);
  }

  private drainWaiters(): void {
    while (this.waiters.length > 0 && this.permitsInUse() < this.limit()) {
      const waiter = this.waiters.shift()!;
      if (!waiter.active) continue;
      waiter.active = false;
      waiter.signal?.removeEventListener('abort', waiter.abort);
      this.recordCompletedWait(waiter);
      waiter.resolve(this.reserve(false));
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
    if (waiter.metricsEpoch !== this.metricsEpoch) return;
    const durationMs = Math.max(0, this.nowMs() - waiter.startedAtMs);
    this.capacityWaitDurationSeconds += durationMs / 1_000;
  }

  private evaluatePressure(): void {
    const permitsInUse = this.permitsInUse();
    const next = this.pressureStateValue === 'running'
      ? permitsInUse >= this.configuration.pausePermitCount ? 'paused' : 'running'
      : permitsInUse <= this.configuration.resumePermitCount ? 'running' : 'paused';
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
    return Math.max(0, nowMs - this.pauseStartedAtMs) / 1_000;
  }

  private cumulativePauseDurationAt(nowMs: number): number {
    if (this.cumulativePauseStartedAtMs === undefined) return 0;
    return Math.max(0, nowMs - this.cumulativePauseStartedAtMs) / 1_000;
  }
}

function validatePressureThresholds(pause: number, resume: number): void {
  if (!Number.isInteger(pause) || pause < 1 || pause > 100) {
    throw new RangeError('pauseThresholdPercent must be an integer in the range 1..100.');
  }
  if (!Number.isInteger(resume) || resume < 0 || resume > 99) {
    throw new RangeError('resumeThresholdPercent must be an integer in the range 0..99.');
  }
  if (resume >= pause) {
    throw new RangeError('resumeThresholdPercent must be less than pauseThresholdPercent.');
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
    return count > BigInt(Number.MAX_SAFE_INTEGER)
      ? Number.MAX_SAFE_INTEGER
      : Number(count);
  } catch {
    return undefined;
  }
}

function positiveSafeInteger(value: number | undefined): number | undefined {
  return value !== undefined && Number.isSafeInteger(value) && value > 0
    ? value
    : undefined;
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
