import { ZLinkApplicationHwmProfile } from '../../contracts';
import type { ZLinkInboundDispatchStatus } from '../../contracts';
import type { ZLinkInboundDispatchOptionValues } from
  '../../contracts/Configuration/InboundDispatch';
import { ZLinkConfigurationException } from '../configuration';
import { readFileSync } from 'node:fs';
import { totalmem } from 'node:os';
import { join } from 'node:path';
import { getHeapStatistics } from 'node:v8';

const PROFILE_PERCENT = new Map<ZLinkApplicationHwmProfile, bigint>([
  [ZLinkApplicationHwmProfile.Compact, 2n],
  [ZLinkApplicationHwmProfile.LowLatency, 5n],
  [ZLinkApplicationHwmProfile.Balanced, 10n],
  [ZLinkApplicationHwmProfile.Throughput, 20n]
]);
const COMPLETION_SEND_LIMIT = 65_536n;

export interface ZLinkApplicationHwmMemorySources {
  readonly osLimitBytes?: bigint;
  readonly managedHeapLimitBytes?: bigint;
  readonly physicalMemoryBytes?: bigint;
}

export class ZLinkInboundDispatchBudget {
  private queuedBytes = 0n;
  private activeBytes = 0n;
  private pendingCompletionSends = 0n;
  private activeCompletionSends = 0n;
  private readonly completionWaiters: Array<(() => void) | undefined> = [];
  private completionWaiterHead = 0;
  private completionWaiterCount = 0;
  private readonly pauseListeners = new Set<() => void>();
  private readonly resumeListeners = new Set<() => void>();

  constructor(readonly applicationHwmBytes: bigint) {}

  get receivePaused(): boolean {
    return this.applicationHwmBytes > 0n
      && this.pendingPayloadBytes >= this.applicationHwmBytes;
  }

  get pendingPayloadBytes(): bigint {
    return this.queuedBytes + this.activeBytes;
  }

  enqueue(payloadBytes: bigint): void {
    const wasPaused = this.receivePaused;
    this.queuedBytes += requirePayloadBytes(payloadBytes);
    this.notifyPauseTransition(wasPaused);
  }

  start(payloadBytes: bigint): void {
    const bytes = requirePayloadBytes(payloadBytes);
    if (bytes > this.queuedBytes) {
      throw new Error('Inbound dispatch byte accounting underflow.');
    }
    this.queuedBytes -= bytes;
    this.activeBytes += bytes;
  }

  cancelQueued(payloadBytes: bigint): void {
    const wasPaused = this.receivePaused;
    const bytes = requirePayloadBytes(payloadBytes);
    if (bytes > this.queuedBytes) {
      throw new Error('Inbound dispatch queued byte accounting underflow.');
    }
    this.queuedBytes -= bytes;
    const stillPaused = this.receivePaused;
    this.notifyResume(wasPaused, stillPaused);
  }

  complete(payloadBytes: bigint): void {
    const wasPaused = this.receivePaused;
    const bytes = requirePayloadBytes(payloadBytes);
    if (bytes > this.activeBytes) {
      throw new Error('Inbound dispatch active byte accounting underflow.');
    }
    this.activeBytes -= bytes;
    const stillPaused = this.receivePaused;
    this.notifyResume(wasPaused, stillPaused);
  }

  onPause(listener: () => void): () => void {
    this.pauseListeners.add(listener);
    return () => this.pauseListeners.delete(listener);
  }

  onResume(listener: () => void): () => void {
    this.resumeListeners.add(listener);
    return () => this.resumeListeners.delete(listener);
  }

  private notifyPauseTransition(wasPaused: boolean): void {
    const isPaused = this.receivePaused;
    if (!wasPaused && isPaused) {
      for (const listener of this.pauseListeners) listener();
    }
  }

  private notifyResume(wasPaused: boolean, isPaused: boolean): void {
    if (wasPaused && !isPaused) {
      for (const listener of this.resumeListeners) listener();
    }
  }

  async waitUntilResumed(signal?: AbortSignal): Promise<void> {
    if (!this.receivePaused || signal?.aborted === true) return;
    await new Promise<void>((resolve) => {
      const finish = () => {
        removeResume();
        signal?.removeEventListener('abort', finish);
        resolve();
      };
      const removeResume = this.onResume(finish);
      signal?.addEventListener('abort', finish, { once: true });
      if (!this.receivePaused) finish();
    });
  }

  async acquireCompletionSend(signal?: AbortSignal): Promise<() => void> {
    this.pendingCompletionSends += 1n;
    let admitted = false;
    try {
      if (this.activeCompletionSends >= COMPLETION_SEND_LIMIT) {
        await new Promise<void>((resolve, reject) => {
          const wake = () => {
            signal?.removeEventListener('abort', abort);
            resolve();
          };
          const abort = () => {
            const index = this.completionWaiters.indexOf(wake, this.completionWaiterHead);
            if (index >= 0) {
              this.completionWaiters[index] = undefined;
              this.completionWaiterCount -= 1;
              this.advanceCompletionWaiterHead();
              this.compactCompletionWaiters();
            }
            reject(signal?.reason ?? new Error('Completion send admission was cancelled.'));
          };
          this.completionWaiters.push(wake);
          this.completionWaiterCount += 1;
          signal?.addEventListener('abort', abort, { once: true });
          if (signal?.aborted === true) abort();
        });
      }
      this.activeCompletionSends += 1n;
      admitted = true;
      let released = false;
      return () => {
        if (released) return;
        released = true;
        this.activeCompletionSends -= 1n;
        this.pendingCompletionSends -= 1n;
        this.takeCompletionWaiter()?.();
      };
    } finally {
      if (!admitted) this.pendingCompletionSends -= 1n;
    }
  }

  snapshot(): ZLinkInboundDispatchStatus {
    return {
      applicationHwmBytes: this.applicationHwmBytes,
      pendingPayloadBytes: this.pendingPayloadBytes,
      queuedPayloadBytes: this.queuedBytes,
      activePayloadBytes: this.activeBytes,
      applicationReceivePaused: this.receivePaused,
      pendingCompletionSends: this.pendingCompletionSends,
      completionSendLimit: COMPLETION_SEND_LIMIT
    };
  }

  private takeCompletionWaiter(): (() => void) | undefined {
    this.advanceCompletionWaiterHead();
    const waiter = this.completionWaiters[this.completionWaiterHead];
    if (waiter === undefined) return undefined;
    this.completionWaiters[this.completionWaiterHead] = undefined;
    this.completionWaiterHead += 1;
    this.completionWaiterCount -= 1;
    this.compactCompletionWaiters();
    return waiter;
  }

  private advanceCompletionWaiterHead(): void {
    while (
      this.completionWaiterHead < this.completionWaiters.length
      && this.completionWaiters[this.completionWaiterHead] === undefined
    ) {
      this.completionWaiterHead += 1;
    }
  }

  private compactCompletionWaiters(): void {
    if (this.completionWaiterCount === 0) {
      this.completionWaiters.length = 0;
      this.completionWaiterHead = 0;
      return;
    }
    if (
      this.completionWaiterHead >= 1024
      && this.completionWaiterHead * 2 >= this.completionWaiters.length
    ) {
      this.completionWaiters.splice(0, this.completionWaiterHead);
      this.completionWaiterHead = 0;
    }
  }
}

export function resolveApplicationHwm(
  options: ZLinkInboundDispatchOptionValues,
  memorySources?: ZLinkApplicationHwmMemorySources
): bigint {
  const configured = options.applicationHwmBytes;
  if (configured !== undefined) {
    if (configured < 0n) {
      throw new ZLinkConfigurationException(
        'Application HWM bytes must be zero or positive.'
      );
    }
    return configured;
  }
  //  Spec 06: configured limit, then the smaller of the OS and managed-heap
  //  limits, then total physical memory. Total, not free, stays deterministic.
  const memoryLimit = options.processMemoryLimitBytes
    ?? resolveEffectiveMemoryBudget(memorySources ?? detectMemorySources());
  if (memoryLimit <= 0n) {
    throw new ZLinkConfigurationException(
      'Application HWM Auto sizing requires a positive effective memory budget.'
    );
  }
  const percent = PROFILE_PERCENT.get(options.applicationHwmProfile);
  if (percent === undefined) {
    throw new ZLinkConfigurationException(
      'Application HWM profile is not supported.'
    );
  }
  const resolved = memoryLimit * percent / 100n;
  if (resolved <= 0n) {
    throw new ZLinkConfigurationException(
      'Application HWM Auto sizing produced a non-positive byte limit.'
    );
  }
  return resolved;
}

export function resolveEffectiveMemoryBudget(
  sources: ZLinkApplicationHwmMemorySources
): bigint {
  const candidates = [sources.osLimitBytes, sources.managedHeapLimitBytes]
    .filter((candidate): candidate is bigint => candidate !== undefined && candidate > 0n);
  if (candidates.length > 0) {
    return candidates.reduce((smallest, candidate) =>
      candidate < smallest ? candidate : smallest);
  }
  return sources.physicalMemoryBytes ?? 0n;
}

function detectMemorySources(): ZLinkApplicationHwmMemorySources {
  const osCandidates: bigint[] = [];

  // Node reports 0 when the process is not memory constrained, so only a
  // positive safe integer names a real OS limit.
  const constrained = typeof process.constrainedMemory === 'function'
    ? process.constrainedMemory()
    : 0;
  if (Number.isSafeInteger(constrained) && constrained > 0) {
    osCandidates.push(BigInt(constrained));
  }
  const cgroupPaths = currentCgroupPaths();
  const unifiedLimit = cgroupPaths.unifiedPath === undefined
    ? undefined
    : readFiniteLimit(join('/sys/fs/cgroup', cgroupPaths.unifiedPath, 'memory.max'));
  const memoryLimit = cgroupPaths.memoryPath === undefined
    ? undefined
    : readFiniteLimit(
        join('/sys/fs/cgroup/memory', cgroupPaths.memoryPath, 'memory.limit_in_bytes')
      );
  const cgroupLimit = unifiedLimit ?? memoryLimit;
  if (cgroupLimit !== undefined) osCandidates.push(cgroupLimit);

  const managedHeapLimit = getHeapStatistics().heap_size_limit;
  return {
    osLimitBytes: osCandidates.length === 0
      ? undefined
      : osCandidates.reduce((smallest, candidate) =>
        candidate < smallest ? candidate : smallest),
    managedHeapLimitBytes: Number.isSafeInteger(managedHeapLimit) && managedHeapLimit > 0
      ? BigInt(managedHeapLimit)
      : undefined,
    physicalMemoryBytes: totalPhysicalMemory()
  };
}

function totalPhysicalMemory(): bigint {
  const total = totalmem();
  return Number.isSafeInteger(total) && total > 0 ? BigInt(total) : 0n;
}

function currentCgroupPaths(): {
  readonly unifiedPath?: string;
  readonly memoryPath?: string;
} {
  try {
    let unifiedPath: string | undefined;
    let memoryPath: string | undefined;
    for (const line of readFileSync('/proc/self/cgroup', 'utf8').split(/\r?\n/u)) {
      const separator = line.indexOf(':');
      if (separator < 0) continue;
      const secondSeparator = line.indexOf(':', separator + 1);
      if (secondSeparator < 0) continue;
      const controllers = line.slice(separator + 1, secondSeparator);
      const path = line.slice(secondSeparator + 1).replace(/^\/+/u, '');
      if (controllers === '') {
        unifiedPath = path;
      } else if (controllers.split(',').includes('memory')) {
        memoryPath = path;
      }
    }
    return { unifiedPath, memoryPath };
  } catch {
    return {};
  }
}

function readFiniteLimit(path: string): bigint | undefined {
  try {
    const value = readFileSync(path, 'utf8').trim();
    if (value === '' || value === 'max') return undefined;
    const parsed = BigInt(value);
    return parsed > 0n && parsed < (1n << 63n) ? parsed : undefined;
  } catch {
    return undefined;
  }
}

function requirePayloadBytes(value: bigint): bigint {
  if (value < 0n) throw new RangeError('Payload bytes cannot be negative.');
  return value;
}
