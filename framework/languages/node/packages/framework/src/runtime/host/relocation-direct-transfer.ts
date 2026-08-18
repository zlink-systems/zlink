import {
  RELOCATION_PAYLOAD_CHUNK_COUNT_MAX,
  RELOCATION_STATE_CHUNK_DATA_MAX_BYTES
} from '../foundation/service-stateful-wire-codec';
import { crc32c } from '../foundation/service-relocation-runtime';

/**
 * Conservative chunk size for paths without a negotiated target receive cap.
 * Spec 28 §4.2: when no admission reply announced the target's effective
 * receive chunk limit, the source uses the 32 KiB value that is safe on any
 * deployment.
 */
export const RELOCATION_CONSERVATIVE_CHUNK_LIMIT_BYTES = 32 * 1024;

/**
 * Phase 1 conservative in-flight budget floor. Until Core exposes the
 * per-pipe applied HWM observation API, the effective budget is
 * min(configured, this constant) — spec 28 §5.3 (구현 노트: Phase 1 근사).
 */
export const RELOCATION_PHASE1_CONSERVATIVE_BUDGET_BYTES = 8 * 1024 * 1024;

export interface ZLinkRelocationChunkPlan {
  readonly chunkBytes: number;
  readonly chunkCount: number;
  readonly totalLength: number;
  readonly checksumCrc32c: number;
}

/**
 * Splits one relocation payload into ordered chunks that satisfy the shared
 * wire bounds (chunk data <= 64 MiB, chunk count <= 4096). The configured
 * limit is honoured unless the count bound forces larger chunks.
 */
export function planRelocationChunks(
  payload: Uint8Array,
  configuredChunkLimitBytes: number
): ZLinkRelocationChunkPlan {
  if (!Number.isInteger(configuredChunkLimitBytes) || configuredChunkLimitBytes <= 0) {
    throw new RangeError('relocationPayloadChunkLimitBytes must be a positive integer.');
  }
  const totalLength = payload.byteLength;
  let chunkBytes = Math.min(
    configuredChunkLimitBytes,
    RELOCATION_CONSERVATIVE_CHUNK_LIMIT_BYTES
  );
  if (Math.ceil(totalLength / chunkBytes) > RELOCATION_PAYLOAD_CHUNK_COUNT_MAX) {
    chunkBytes = Math.ceil(totalLength / RELOCATION_PAYLOAD_CHUNK_COUNT_MAX);
  }
  if (chunkBytes > RELOCATION_STATE_CHUNK_DATA_MAX_BYTES) {
    throw new RangeError('Relocation payload exceeds the direct transfer size bound.');
  }
  const chunkCount = Math.max(1, Math.ceil(totalLength / chunkBytes));
  return {
    chunkBytes,
    chunkCount,
    totalLength,
    checksumCrc32c: crc32c(payload)
  };
}

export function relocationChunkAt(
  payload: Uint8Array,
  plan: ZLinkRelocationChunkPlan,
  ordinal: number
): Buffer {
  if (!Number.isInteger(ordinal) || ordinal < 0 || ordinal >= plan.chunkCount) {
    throw new RangeError('Relocation chunk ordinal is out of range.');
  }
  const source = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  return source.subarray(
    ordinal * plan.chunkBytes,
    Math.min(source.byteLength, (ordinal + 1) * plan.chunkBytes)
  );
}

export type ZLinkRelocationChunkAcceptance = 'accepted' | 'duplicate' | 'completed';

/**
 * Target-side assembly buffer for one exact relocation identity. Chunks are
 * copied out of their transport lease immediately; once every chunk arrived
 * the payload checksum is verified exactly once. A checksum mismatch is a
 * non-retryable explicit failure (spec 28 §4.3) and partial payloads are
 * never exposed.
 */
export class ZLinkRelocationPayloadAssembly {
  private readonly buffer: Buffer;
  private nextOrdinal = 0;
  private receivedBytes = 0;
  private outcome?: { readonly payload: Buffer } | { readonly error: Error };
  private readonly waiters: Array<{
    resolve: (payload: Buffer) => void;
    reject: (error: Error) => void;
  }> = [];

  constructor(
    readonly totalLength: number,
    readonly chunkCount: number,
    readonly checksumCrc32c: number
  ) {
    if (!Number.isSafeInteger(totalLength) || totalLength < 0) {
      throw new RangeError('Relocation payload total length is invalid.');
    }
    if (!Number.isInteger(chunkCount) || chunkCount < 0
      || chunkCount > RELOCATION_PAYLOAD_CHUNK_COUNT_MAX
      || (totalLength > 0 && chunkCount === 0)) {
      throw new RangeError('Relocation payload chunk count is invalid.');
    }
    this.buffer = Buffer.alloc(totalLength);
    if (chunkCount === 0) this.finish();
  }

  get completed(): boolean {
    return this.outcome !== undefined && 'payload' in this.outcome;
  }

  /** Copies one in-order chunk into the assembly buffer. */
  accept(ordinal: number, data: Uint8Array): ZLinkRelocationChunkAcceptance {
    if (this.outcome !== undefined) {
      if ('error' in this.outcome) throw this.outcome.error;
      return ordinal < this.chunkCount ? 'duplicate' : this.conflict(
        `Relocation chunk ordinal ${ordinal} arrived after assembly completed.`
      );
    }
    if (ordinal < this.nextOrdinal) return 'duplicate';
    if (ordinal !== this.nextOrdinal || ordinal >= this.chunkCount) {
      return this.conflict(
        `Relocation chunk ordinal ${ordinal} does not continue the ordered assembly.`
      );
    }
    if (this.receivedBytes + data.byteLength > this.totalLength) {
      return this.conflict('Relocation chunks exceed the declared payload length.');
    }
    if (ordinal === this.chunkCount - 1
      && this.receivedBytes + data.byteLength !== this.totalLength) {
      return this.conflict('Relocation chunks do not cover the declared payload length.');
    }
    this.buffer.set(data, this.receivedBytes);
    this.receivedBytes += data.byteLength;
    this.nextOrdinal += 1;
    if (this.nextOrdinal === this.chunkCount) this.finish();
    return this.outcome !== undefined && 'payload' in this.outcome
      ? 'completed'
      : 'accepted';
  }

  /** Resolves with the verified payload or rejects with the explicit failure. */
  payload(): Promise<Buffer> {
    if (this.outcome !== undefined) {
      return 'payload' in this.outcome
        ? Promise.resolve(this.outcome.payload)
        : Promise.reject(this.outcome.error);
    }
    return new Promise((resolve, reject) => {
      this.waiters.push({ resolve, reject });
    });
  }

  /** Ends the assembly with an explicit failure (identity conflict, abort). */
  fail(message: string): Error {
    if (this.outcome !== undefined) {
      return 'error' in this.outcome ? this.outcome.error : new Error(message);
    }
    const error = new Error(message);
    this.outcome = { error };
    for (const waiter of this.waiters.splice(0)) waiter.reject(error);
    return error;
  }

  private finish(): void {
    if (crc32c(this.buffer) !== this.checksumCrc32c) {
      // Spec 28 §9: an assembled-checksum mismatch on an ordered reliable
      // connection signals an implementation defect or memory corruption.
      // It is an explicit, non-retryable failure and the partial payload is
      // never used for Restore.
      throw this.fail('Relocation payload checksum mismatch after chunk assembly.');
    }
    this.outcome = { payload: this.buffer };
    for (const waiter of this.waiters.splice(0)) waiter.resolve(this.buffer);
  }

  private conflict(message: string): never {
    throw this.fail(message);
  }
}

/**
 * Byte budget for relocation payload bytes concurrently in flight on one
 * scope (per-peer connection or whole node). A zero limit disables the
 * budget. Acquisition larger than the whole budget is still admitted when
 * nothing else is in flight, so no payload size is unable to start.
 */
export class ZLinkRelocationInFlightBudget {
  private inFlight = 0;
  private readonly waiters: Array<() => void> = [];

  constructor(readonly limitBytes: number) {
    if (!Number.isSafeInteger(limitBytes) || limitBytes < 0) {
      throw new RangeError('Relocation in-flight budget must be a non-negative integer.');
    }
  }

  get inFlightBytes(): number {
    return this.inFlight;
  }

  /** Waits for room, charges the bytes, and returns the exactly-once release. */
  async acquire(bytes: number, signal?: AbortSignal): Promise<() => void> {
    if (!Number.isSafeInteger(bytes) || bytes < 0) {
      throw new RangeError('Relocation budget charge must be a non-negative integer.');
    }
    while (this.limitBytes !== 0
      && this.inFlight > 0
      && this.inFlight + bytes > this.limitBytes) {
      await this.waitForRelease(signal);
    }
    this.inFlight += bytes;
    let released = false;
    return () => {
      if (released) return;
      released = true;
      this.inFlight -= bytes;
      for (const waiter of this.waiters.splice(0)) waiter();
    };
  }

  /** Pre-seal admission wait: resolves while the budget has any headroom. */
  async waitForHeadroom(signal?: AbortSignal): Promise<void> {
    while (this.limitBytes !== 0 && this.inFlight >= this.limitBytes) {
      await this.waitForRelease(signal);
    }
  }

  private waitForRelease(signal?: AbortSignal): Promise<void> {
    signal?.throwIfAborted();
    return new Promise((resolve, reject) => {
      const waiter = () => {
        signal?.removeEventListener('abort', aborted);
        resolve();
      };
      const aborted = () => {
        const index = this.waiters.indexOf(waiter);
        if (index >= 0) this.waiters.splice(index, 1);
        reject(signal?.reason as Error);
      };
      signal?.addEventListener('abort', aborted, { once: true });
      this.waiters.push(waiter);
    });
  }
}

/** Effective Phase 1 budget: min(configured, conservative floor); 0 = off. */
export function effectiveRelocationBudgetBytes(configuredBytes: number): number {
  if (configuredBytes === 0) return 0;
  return Math.min(configuredBytes, RELOCATION_PHASE1_CONSERVATIVE_BUDGET_BYTES);
}
