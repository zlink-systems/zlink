import { availableParallelism } from 'node:os';
import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';

export type ApplicationJobQueueProfileValue =
  | 'compact'
  | 'low_latency'
  | 'balanced'
  | 'throughput';

export interface ApplicationJobQueueOptions {
  readonly profile?: ApplicationJobQueueProfileValue;
  readonly maxQueuedApplicationJobs?: bigint;
}

export interface ApplicationJobQueueConfiguration {
  readonly configuredProfile: ApplicationJobQueueProfileValue;
  readonly configuredManualMax?: bigint;
  readonly effectiveProcessorCount: bigint;
  readonly effectiveMaxQueuedApplicationJobs: bigint;
}

export interface ApplicationJobQueueSnapshot extends ApplicationJobQueueConfiguration {
  readonly reservedSupplyPermits: bigint;
  readonly queuedApplicationJobs: bigint;
  readonly permitsInUse: bigint;
  readonly peakPermitsInUse: bigint;
  readonly capacityWaiters: bigint;
  readonly capacityWaitCount: bigint;
  readonly capacityWaitDurationSeconds: number;
}

const MAX_QUEUED_APPLICATION_JOBS = 2_147_483_647n;
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

  return Object.freeze({
    configuredProfile,
    configuredManualMax,
    effectiveProcessorCount: processors,
    effectiveMaxQueuedApplicationJobs: effectiveMax
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
  private readonly waiters: CapacityWaiter[] = [];

  constructor(
    private readonly configuration: ApplicationJobQueueConfiguration,
    private readonly nowMs: () => number = () => performance.now()
  ) {}

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
        active: true
      };
      this.waiters.push(waiter);
      signal?.addEventListener('abort', waiter.abort, { once: true });
      this.drainWaiters();
    });
  }

  snapshot(): ApplicationJobQueueSnapshot {
    const permitsInUse = this.permitsInUse();
    return Object.freeze({
      ...this.configuration,
      reservedSupplyPermits: this.reservedSupplyPermits,
      queuedApplicationJobs: this.queuedApplicationJobs,
      permitsInUse,
      peakPermitsInUse: this.peakPermitsInUse,
      capacityWaiters: BigInt(this.waiters.length),
      capacityWaitCount: this.capacityWaitCount,
      capacityWaitDurationSeconds: this.capacityWaitDurationSeconds
    });
  }

  resetMetrics(): void {
    this.peakPermitsInUse = this.permitsInUse();
    this.capacityWaitCount = 0n;
    this.capacityWaitDurationSeconds = 0;
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
  }

  private limit(): bigint {
    return this.configuration.effectiveMaxQueuedApplicationJobs;
  }

  private permitsInUse(): bigint {
    return this.reservedSupplyPermits + this.queuedApplicationJobs;
  }

  private reserve(): ApplicationJobQueuePermit {
    this.reservedSupplyPermits += 1n;
    const current = this.permitsInUse();
    if (current > this.peakPermitsInUse) this.peakPermitsInUse = current;
    return new ApplicationJobQueuePermit(this);
  }

  private drainWaiters(): void {
    while (this.waiters.length > 0 && this.permitsInUse() < this.limit()) {
      const waiter = this.waiters.shift()!;
      if (!waiter.active) continue;
      waiter.active = false;
      waiter.signal?.removeEventListener('abort', waiter.abort);
      this.recordCompletedWait(waiter.startedAtMs);
      waiter.resolve(this.reserve());
    }
  }

  private cancel(waiter: CapacityWaiter): void {
    if (!waiter.active) return;
    waiter.active = false;
    const index = this.waiters.indexOf(waiter);
    if (index >= 0) this.waiters.splice(index, 1);
    waiter.signal?.removeEventListener('abort', waiter.abort);
    this.recordCompletedWait(waiter.startedAtMs);
    waiter.reject(waiter.signal === undefined ? abortError() : abortReason(waiter.signal));
    this.drainWaiters();
  }

  private recordCompletedWait(startedAtMs: number): void {
    this.capacityWaitCount += 1n;
    const durationMs = Math.max(0, this.nowMs() - startedAtMs);
    this.capacityWaitDurationSeconds += durationMs / 1_000;
  }
}

export function nodeEffectiveProcessorCount(explicitExecutorMaximum?: number): bigint {
  const count = availableParallelism();
  const runtimeCount = Number.isSafeInteger(count) && count > 0 ? count : 1;
  const effective = explicitExecutorMaximum === undefined
    ? runtimeCount
    : Math.min(runtimeCount, explicitExecutorMaximum);
  return BigInt(Number.isSafeInteger(effective) && effective > 0 ? effective : 1);
}

function abortReason(signal: AbortSignal): unknown {
  return signal.reason ?? abortError();
}

function abortError(): Error {
  const error = new Error('Application job queue capacity wait was aborted.');
  error.name = 'AbortError';
  return error;
}
