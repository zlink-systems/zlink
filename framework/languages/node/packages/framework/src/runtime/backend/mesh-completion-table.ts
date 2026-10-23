import { ZLinkBufferMessage as Message } from './runtime-message';
import type { Message as ZLinkMessage } from '../../contracts/Common/Message';
import type {
  MeshOperationId,
  ReceiveKindData,
  ReceiveRecord
} from '../foundation/service-runtime-contracts';
import { operationIdentityKey, validateOperationIdentity } from '../foundation/operation-identity';

export interface ZLinkMeshCompletionDiagnostic {
  readonly kind: 'unknownOrLate';
  readonly operationId: MeshOperationId;
  readonly terminalResult: number;
}

export interface ZLinkMeshCompletion {
  readonly terminalResult: number;
  readonly failureErrno: number;
  readonly operationKind: number;
  readonly kindData: ReceiveKindData | null;
  readonly parts: Message[];
}

const COMPLETION_SLOT_BLOCK_SIZE = 64;
const COMPLETION_HASH_BUCKETS_PER_SLOT = 2;
const COMPLETION_HASH_WORD_SCALE = 0x1_0000;
const COMPLETION_HASH_MULTIPLIER = 0x9e37_79b1;
const COMPLETION_ABORT_OPTIONS = Object.freeze({ once: true });

class CompletionSlot {
  generation = 0;
  next = -1;
  high?: bigint;
  low?: bigint;
  hashNext = -1;
  resolve?: (completion: ZLinkMeshCompletion) => void;
  reject?: (error: unknown) => void;
  signal?: AbortSignal;

  constructor(readonly abort: () => void) {}
}

export type ZLinkMeshCompletionTerminal = Pick<ReceiveRecord, 'operationId' | 'terminalResult'>;
export type ZLinkMeshCompletionHandler = (
  terminal: ZLinkMeshCompletionTerminal,
  materialize: () => ReceiveRecord
) => void;

export class ZLinkMeshCompletionTable {
  private buckets = new Int32Array(0);
  private readonly slots: CompletionSlot[] = [];
  private free = -1;
  private activeCount = 0;
  private disposed = false;

  constructor(private readonly onDiagnostic?: (diagnostic: ZLinkMeshCompletionDiagnostic) => void) {
    this.prepareSlots();
  }

  private prepareSlots(): void {
    const start = this.slots.length;
    for (let index = start; index < start + COMPLETION_SLOT_BLOCK_SIZE; index++) {
      const slot = new CompletionSlot(() => {
        const reject = slot.reject;
        const signal = slot.signal;
        if (reject === undefined) return;
        this.releaseSlot(index);
        reject(signal?.reason ?? new DOMException('The operation was aborted.', 'AbortError'));
      });
      slot.next = this.free;
      this.slots.push(slot);
      this.free = index;
    }
    // New slots hold no identity yet. Only a larger bucket array needs the
    // pending identities placed again; it doubles, so growth to N pending
    // operations re-indexes O(N) slots in total.
    const minimumBuckets = this.slots.length * COMPLETION_HASH_BUCKETS_PER_SLOT;
    if (minimumBuckets <= this.buckets.length) return;
    this.buckets = new Int32Array(2 ** Math.ceil(Math.log2(minimumBuckets))).fill(-1);
    for (let index = 0; index < this.slots.length; index++) {
      const slot = this.slots[index]!;
      if (slot.high === undefined) continue;
      this.indexSlot(index);
    }
  }

  private bucket(high: bigint, low: bigint): number {
    const highWord = Number(high);
    const lowWord = Number(low);
    // Bucket selection may collide; the stored 128-bit identity is compared exactly.
    const hash =
      Math.imul(
        highWord ^ Math.trunc(highWord / COMPLETION_HASH_WORD_SCALE),
        COMPLETION_HASH_MULTIPLIER
      ) ^
      lowWord ^
      Math.trunc(lowWord / COMPLETION_HASH_WORD_SCALE);
    return hash & (this.buckets.length - 1);
  }

  private indexSlot(index: number): void {
    const slot = this.slots[index]!;
    const bucket = this.bucket(slot.high!, slot.low!);
    slot.hashNext = this.buckets[bucket]!;
    this.buckets[bucket] = index;
  }

  private findSlot(operationId: MeshOperationId): number {
    validateOperationIdentity(operationId);
    for (
      let index = this.buckets[this.bucket(operationId.high, operationId.low)]!;
      index !== -1;
      index = this.slots[index]!.hashNext
    ) {
      const slot = this.slots[index]!;
      if (slot.high === operationId.high && slot.low === operationId.low) return index;
    }
    return -1;
  }

  private releaseSlot(index: number): void {
    const slot = this.slots[index]!;
    if (slot.high !== undefined) {
      const bucket = this.bucket(slot.high, slot.low!);
      let previous = -1;
      let current = this.buckets[bucket]!;
      while (current !== index) {
        previous = current;
        current = this.slots[current]!.hashNext;
      }
      if (previous === -1) this.buckets[bucket] = slot.hashNext;
      else this.slots[previous]!.hashNext = slot.hashNext;
    }
    slot.signal?.removeEventListener('abort', slot.abort);
    slot.high = undefined;
    slot.low = undefined;
    slot.hashNext = -1;
    slot.signal = undefined;
    slot.resolve = undefined;
    slot.reject = undefined;
    slot.next = this.free;
    this.free = index;
    this.activeCount--;
  }

  /**
   * Reserves the dispatcher slot before transport submission. The backend
   * observes native terminals through Promises, so identity binding finishes
   * before the terminal callback can run, without an infrastructure pump.
   */
  submit(operation: () => MeshOperationId, signal?: AbortSignal): Promise<ZLinkMeshCompletion> {
    if (this.isDisposed()) {
      return Promise.reject(new Error('Mesh completion table is disposed.'));
    }
    if (signal?.aborted === true) {
      return Promise.reject(
        signal.reason ?? new DOMException('The operation was aborted.', 'AbortError')
      );
    }
    return new Promise((resolve, reject) => {
      if (this.free === -1) this.prepareSlots();
      const index = this.free;
      const slot = this.slots[index]!;
      this.free = slot.next;
      const generation = ++slot.generation;
      slot.resolve = resolve;
      slot.reject = reject;
      slot.signal = signal;
      this.activeCount++;
      try {
        signal?.addEventListener('abort', slot.abort, COMPLETION_ABORT_OPTIONS);
        const operationId = operation();
        // A reentrant abort may release and reuse this slot during submission.
        if (slot.generation !== generation || slot.resolve !== resolve) return;
        if (this.findSlot(operationId) !== -1) {
          throw new Error(
            `Mesh operation '${operationIdentityKey(operationId)}' is already pending.`
          );
        }
        slot.high = operationId.high;
        slot.low = operationId.low;
        this.indexSlot(index);
      } catch (error) {
        if (slot.generation === generation && slot.resolve === resolve) this.releaseSlot(index);
        reject(error);
      }
    });
  }

  private isDisposed(): boolean {
    return this.disposed;
  }

  complete(record: ReceiveRecord): void;
  complete(record: ZLinkMeshCompletionTerminal, materialize: () => ReceiveRecord): void;
  complete(record: ZLinkMeshCompletionTerminal, materialize?: () => ReceiveRecord): void {
    const index = this.findSlot(record.operationId);
    if (index === -1) {
      this.onDiagnostic?.({
        kind: 'unknownOrLate',
        operationId: Object.freeze({ ...record.operationId }),
        terminalResult: record.terminalResult
      });
      return;
    }
    const slot = this.slots[index]!;
    const resolve = slot.resolve!;
    const reject = slot.reject!;
    this.releaseSlot(index);
    try {
      resolve(
        retainCompletion(materialize === undefined ? (record as ReceiveRecord) : materialize())
      );
    } catch (error) {
      reject(error);
    }
  }

  dispose(reason: unknown = new Error('Mesh completion table is disposed.')): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    for (let index = 0; index < this.slots.length; index++) {
      const reject = this.slots[index]!.reject;
      if (reject === undefined) continue;
      this.releaseSlot(index);
      reject(reason);
    }
  }

  get pendingCount(): number {
    return this.activeCount;
  }
}

function retainCompletion(record: ReceiveRecord): ZLinkMeshCompletion {
  const parts = retainCompletionParts(record.parts);
  // The retention pass verifies every part is already a managed runtime message.
  if (parts === record.parts) return record as unknown as ZLinkMeshCompletion;
  return {
    terminalResult: record.terminalResult,
    failureErrno: record.failureErrno,
    operationKind: record.operationKind,
    kindData: record.kindData,
    parts
  };
}

function retainCompletionParts(parts: readonly ZLinkMessage[]): Message[] {
  // Reuse managed messages and wrap binding payloads in the runtime representation.
  let retained: Message[] | undefined;
  for (let index = 0; index < parts.length; index++) {
    const part = parts[index]!;
    if (!(part instanceof Message)) {
      retained ??= parts.slice() as Message[];
      retained[index] = Message.fromOwned(Message.retainPayload(part));
    }
  }
  return retained ?? (parts as Message[]);
}

export function closeMeshCompletion(completion: ZLinkMeshCompletion): void {
  closeParts(completion.parts);
}

function closeParts(parts: readonly Message[]): void {
  for (let index = 0; index < parts.length; index++) parts[index]!.close();
}
