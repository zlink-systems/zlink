// SPDX-License-Identifier: MPL-2.0

/** An automatic high-water-mark sizing profile trading memory, latency, and throughput. */
export const AutoHwmProfile = Object.freeze({
  Compact: 0,
  LowLatency: 1,
  Balanced: 2,
  Throughput: 3
} as const);
export type AutoHwmProfileValue = typeof AutoHwmProfile[keyof typeof AutoHwmProfile];

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
  /** The profile used to size high-water marks automatically. */
  autoHwmProfile: AutoHwmProfileValue;
  /** The byte planning unit used when auto-sizing high-water marks; 0n selects the socket default. */
  autoHwmMsgUnitBytes: bigint;
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
  /** Close the context and release its resources, terminating anything still open under it. */
  close(): void;
}
