// SPDX-License-Identifier: MPL-2.0

/** An automatic high-water-mark sizing profile trading memory, latency, and throughput. */
export const AutoHwmProfile = Object.freeze({
  Compact: 0,
  LowLatency: 1,
  Balanced: 2,
  Throughput: 3
} as const);
export type AutoHwmProfileValue = typeof AutoHwmProfile[keyof typeof AutoHwmProfile];

/** Immutable Core Auto HWM ABI-v1 context budget snapshot. */
export interface CoreHwmBudgetSnapshot {
  readonly abiVersion: number;
  readonly structSize: number;
  readonly budgetGeneration: bigint;
  readonly measurementEpoch: bigint;
  readonly configuredMemoryLimitBytes: bigint;
  readonly runtimeMemoryLimitBytes: bigint;
  readonly resolvedMemoryLimitBytes: bigint;
  readonly configuredCoreBudgetBytes: bigint;
  readonly effectiveCoreBudgetBytes: bigint;
  readonly totalPlannedHwmBytes: bigint;
  readonly totalAppliedHwmBytes: bigint;
  readonly manualReservedHwmBytes: bigint;
  readonly coreQueueAccountedBytes: bigint;
  readonly applicationAccountedBytes: bigint;
  readonly currentAccountedBytes: bigint;
  readonly provisionalAccountedBytes: bigint;
  readonly peakAccountedBytes: bigint;
  readonly completionCurrentAccountedBytes: bigint;
  readonly completionPeakAccountedBytes: bigint;
  readonly completionPendingMessageCount: bigint;
  readonly totalMessagingAccountedBytes: bigint;
  readonly monitorQueueAppliedHwmBytes: bigint;
  readonly monitorQueueAccountedBytes: bigint;
  readonly totalInstanceAppliedHwmBytes: bigint;
  readonly totalInstanceAccountedBytes: bigint;
  readonly oversizeAdmissionCount: bigint;
  readonly largestOversizeMessageBytes: bigint;
  readonly activeDirectionalQueueCount: bigint;
  readonly activeCompletionDirectionalQueueCount: bigint;
  readonly activeSendQueueCount: bigint;
  readonly activeReceiveQueueCount: bigint;
  readonly outstandingApplicationLeaseCount: bigint;
  readonly retiredQueueCount: bigint;
  readonly deferredOriginCreditBytes: bigint;
  readonly unlimitedManualQueueCount: bigint;
  readonly blockedRatioPpm: number;
  readonly flags: number;
  readonly reservedUInt64: readonly bigint[];
  readonly budgetPlanningActive: boolean;
  readonly budgetInsufficient: boolean;
  readonly aggregateHwmValid: boolean;
  readonly aggregateOverflow: boolean;
}

/** Context-wide options governing the I/O threads and defaults shared by every socket. */
export interface ContextOptions {
  /** The number of background I/O threads serving the context. */
  ioThreads: number;
  /** The maximum number of sockets the context may create. */
  maxSockets: number;
  /** The largest value `maxSockets` may take on this build. */
  readonly socketLimit: number;
  /** The default maximum inbound message size, in bytes, for new sockets. */
  maxMsgSize: number;
  /** The size of the context's message worker thread pool. */
  readonly msgTSize: number;
  /** The OS scheduling priority of the context's I/O threads. */
  threadPriority: number;
  /** The OS scheduling policy of the context's I/O threads. */
  threadSchedulingPolicy: number;
  /** Whether the context blocks on termination until queued messages are sent. */
  blocky: boolean;
  /** Whether high-water marks are sized automatically. */
  autoHwmEnabled: boolean;
  /** The minimum delay, in ms, between automatic high-water-mark recalculations. */
  autoHwmRecalcDebounceMs: number;
  /** The profile used to divide the Core memory budget. */
  coreHwmProfile: AutoHwmProfileValue;
  /** Explicit context memory limit in bytes; 0n selects automatic detection. */
  coreHwmMemoryLimitBytes: bigint;
  /** Explicit context-wide Core budget in bytes; 0n derives it from the memory limit. */
  coreHwmBudgetBytes: bigint;
  /** The prefix applied to the names of threads the context creates. */
  threadNamePrefix: string;
  /** Pin the context's I/O threads to also run on CPU core `cpu`. */
  addThreadAffinity(cpu: number): void;
  /** Remove CPU core `cpu` from the context's I/O thread affinity. */
  removeThreadAffinity(cpu: number): void;
}

/** A messaging context: the factory and owner of sockets and event sources. */
export interface Context {
  /** The context-wide options facade. */
  readonly options: ContextOptions;
  /** Terminate the context, interrupting blocking operations on its sockets without closing them. */
  shutdown(): void;
  /** Recompute automatic high-water marks for the context's sockets immediately. */
  recalculateAutoHwm(): void;
  /** Return an immutable value snapshot of Core's context-wide HWM budget state. */
  getCoreHwmBudgetSnapshot(): CoreHwmBudgetSnapshot;
  /** Reset epoch metrics while preserving current HWM budget gauges. */
  resetCoreHwmBudgetMetrics(): void;
  /** Close the context and release its resources, terminating anything still open under it. */
  close(): void;
}
