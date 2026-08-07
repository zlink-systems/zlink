import { ZLinkBufferMessage as Message } from './runtime-message';
import type {
  MeshOperationId,
  ReceiveKindData,
  ReceiveRecord
} from '../foundation/service-runtime-contracts';

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
    if (this.disposed) {
      return Promise.reject(new Error('Mesh completion table is disposed.'));
    }
    if (signal?.aborted === true) {
      return Promise.reject(
        signal.reason ?? new DOMException('The operation was aborted.', 'AbortError')
      );
    }
    const operationId = operation();
    if (this.disposed) {
      return Promise.reject(new Error('Mesh completion table is disposed.'));
    }
    const key = operationKey(operationId);
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

  complete(record: ReceiveRecord): void {
    if (this.disposed) return;
    const key = operationKey(record.operationId);
    const pending = this.pending.get(key);
    if (pending === undefined) return;
    const completion = copyCompletion(record);
    this.pending.delete(key);
    pending.removeAbort?.();
    pending.resolve(completion);
  }

  dispose(reason: unknown = new Error('Mesh completion table is disposed.')): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    for (const pending of this.pending.values()) {
      pending.removeAbort?.();
      pending.reject(reason);
    }
    this.pending.clear();
  }
}

function copyCompletion(record: ReceiveRecord): ZLinkMeshCompletion {
  return {
    terminalResult: record.terminalResult,
    failureErrno: record.failureErrno,
    operationKind: record.operationKind,
    kindData: record.kindData,
    parts: record.parts.map((part) => Message.from(Buffer.from(part.data())))
  };
}

function operationKey(operationId: MeshOperationId): string {
  return `${operationId.high.toString(16)}:${operationId.low.toString(16)}`;
}

export function closeMeshCompletion(completion: ZLinkMeshCompletion): void {
  closeParts(completion.parts);
}

function closeParts(parts: readonly Message[]): void {
  for (const part of parts) {
    part.close();
  }
}
