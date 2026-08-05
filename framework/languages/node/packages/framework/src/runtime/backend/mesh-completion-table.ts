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
  private readonly arrived = new Map<string, ZLinkMeshCompletion>();
  private disposed = false;

  wait(operationId: MeshOperationId, signal?: AbortSignal): Promise<ZLinkMeshCompletion> {
    const key = operationKey(operationId);
    const arrived = this.arrived.get(key);
    if (arrived !== undefined) {
      this.arrived.delete(key);
      return Promise.resolve(arrived);
    }
    if (this.disposed) {
      return Promise.reject(new Error('Mesh completion table is disposed.'));
    }
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
    const key = operationKey(record.operationId);
    const completion: ZLinkMeshCompletion = {
      terminalResult: record.terminalResult,
      failureErrno: record.failureErrno,
      operationKind: record.operationKind,
      kindData: record.kindData,
      parts: record.parts.map((part) => Message.from(Buffer.from(part.data())))
    };
    const pending = this.pending.get(key);
    if (pending === undefined) {
      this.arrived.set(key, completion);
      return;
    }
    this.pending.delete(key);
    pending.removeAbort?.();
    pending.resolve(completion);
  }

  dispose(reason: unknown = new Error('Mesh completion table is disposed.')): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    for (const completion of this.arrived.values()) {
      closeParts(completion.parts);
    }
    this.arrived.clear();
    for (const pending of this.pending.values()) {
      pending.removeAbort?.();
      pending.reject(reason);
    }
    this.pending.clear();
  }
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
