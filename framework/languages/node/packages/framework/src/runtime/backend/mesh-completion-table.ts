import { ZLinkBufferMessage as Message } from './runtime-message';
import type {
  MeshOperationId,
  ReceiveKindData,
  ReceiveRecord
} from '../foundation/service-runtime-contracts';
import { operationIdentityKey } from '../foundation/operation-identity';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts';

export const ZLINK_MESH_COMPLETION_CAPACITY = 4_096;

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

interface PendingCompletion {
  readonly resolve: (completion: ZLinkMeshCompletion) => void;
  readonly reject: (error: unknown) => void;
  readonly removeAbort?: () => void;
}

export class ZLinkMeshCompletionTable {
  private readonly pending = new Map<string, PendingCompletion>();
  private disposed = false;

  constructor(
    private readonly maxPendingOperations = ZLINK_MESH_COMPLETION_CAPACITY,
    private readonly onDiagnostic?: (diagnostic: ZLinkMeshCompletionDiagnostic) => void
  ) {
    if (!Number.isSafeInteger(maxPendingOperations) || maxPendingOperations <= 0) {
      throw new RangeError('maxPendingOperations must be a positive safe integer.');
    }
  }

  /**
   * Submits and registers without yielding control to the event loop. Mesh
   * completion dispatch is asynchronous, so a completion cannot overtake the
   * registration. The table therefore does not need to retain responses that
   * arrived before their waiter.
   */
  submit(
    operation: () => MeshOperationId,
    signal?: AbortSignal
  ): Promise<ZLinkMeshCompletion> {
    if (this.isDisposed()) {
      return Promise.reject(new Error('Mesh completion table is disposed.'));
    }
    if (signal?.aborted === true) {
      return Promise.reject(
        signal.reason ?? new DOMException('The operation was aborted.', 'AbortError')
      );
    }
    if (this.pending.size >= this.maxPendingOperations) {
      return Promise.reject(new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.CapacityExceeded,
        `Mesh completion capacity ${this.maxPendingOperations} is exhausted.`
      ));
    }
    const operationId = operation();
    // The backend submission callback can synchronously re-enter `dispose()`.
    // TypeScript's control-flow analysis cannot observe mutation through it.
    if (this.disposed) {
      return Promise.reject(new Error('Mesh completion table is disposed.'));
    }
    const key = operationIdentityKey(operationId);
    if (this.pending.has(key)) {
      return Promise.reject(new Error(`Mesh operation '${key}' is already pending.`));
    }
    return new Promise((resolve, reject) => {
      const abort = () => {
        this.pending.delete(key);
        reject(signal?.reason ?? new DOMException('The operation was aborted.', 'AbortError'));
      };
      if (signal?.aborted === true) {
        abort();
        return;
      }
      signal?.addEventListener('abort', abort, { once: true });
      this.pending.set(key, {
        resolve,
        reject,
        removeAbort: signal === undefined
          ? undefined
          : () => signal.removeEventListener('abort', abort)
      });
    });
  }

  private isDisposed(): boolean {
    return this.disposed;
  }

  complete(record: ReceiveRecord): void {
    const key = operationIdentityKey(record.operationId);
    const pending = this.pending.get(key);
    if (pending === undefined) {
      this.onDiagnostic?.({
        kind: 'unknownOrLate',
        operationId: Object.freeze({ ...record.operationId }),
        terminalResult: record.terminalResult
      });
      return;
    }
    this.pending.delete(key);
    pending.removeAbort?.();
    try {
      pending.resolve(copyCompletion(record));
    } catch (error) {
      pending.reject(error);
    }
  }

  dispose(reason: unknown = new Error('Mesh completion table is disposed.')): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    const pending = [...this.pending.values()];
    this.pending.clear();
    for (const entry of pending) {
      entry.removeAbort?.();
    }
    for (const entry of pending) {
      entry.reject(reason);
    }
  }

  get pendingCount(): number {
    return this.pending.size;
  }

}

function copyCompletion(record: ReceiveRecord): ZLinkMeshCompletion {
  return {
    terminalResult: record.terminalResult,
    failureErrno: record.failureErrno,
    operationKind: record.operationKind,
    kindData: record.kindData,
    parts: record.parts.map((part) => Message.fromOwned(Buffer.from(part.data())))
  };
}

export function closeMeshCompletion(completion: ZLinkMeshCompletion): void {
  closeParts(completion.parts);
}

function closeParts(parts: readonly Message[]): void {
  for (const part of parts) {
    part.close();
  }
}
