import { ZLinkBufferMessage as Message } from './runtime-message';
import type { Message as ZLinkMessage } from '../../contracts/Common/Message';
import type {
  MeshOperationId,
  ReceiveKindData,
  ReceiveRecord
} from '../foundation/service-runtime-contracts';
import { operationIdentityKey } from '../foundation/operation-identity';

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

export type ZLinkMeshCompletionTerminal = Pick<ReceiveRecord, 'operationId' | 'terminalResult'>;
export type ZLinkMeshCompletionHandler = (
  terminal: ZLinkMeshCompletionTerminal,
  materialize: () => ReceiveRecord
) => void;

export class ZLinkMeshCompletionTable {
  private readonly pending = new Map<string | symbol, PendingCompletion>();
  private disposed = false;

  constructor(
    private readonly onDiagnostic?: (diagnostic: ZLinkMeshCompletionDiagnostic) => void
  ) {}

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
      let key: string | symbol = Symbol();
      const abort = () => {
        this.pending.delete(key);
        reject(signal?.reason ?? new DOMException('The operation was aborted.', 'AbortError'));
      };
      if (signal?.aborted === true) {
        abort();
        return;
      }
      signal?.addEventListener('abort', abort, { once: true });
      const entry: PendingCompletion = {
        resolve,
        reject,
        removeAbort:
          signal === undefined ? undefined : () => signal.removeEventListener('abort', abort)
      };
      this.pending.set(key, entry);
      try {
        const operationId = operation();
        // dispose/abort can consume the reserved slot during submission.
        if (!this.pending.delete(key)) return;
        key = operationIdentityKey(operationId);
        if (this.pending.has(key)) {
          throw new Error(`Mesh operation '${key}' is already pending.`);
        }
        this.pending.set(key, entry);
      } catch (error) {
        // Remove only this submission's slot; a duplicate identity retains
        // the original waiter.
        if (this.pending.get(key) === entry) this.pending.delete(key);
        entry.removeAbort?.();
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
      pending.resolve(
        retainCompletion(materialize === undefined ? (record as ReceiveRecord) : materialize())
      );
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

function retainCompletion(record: ReceiveRecord): ZLinkMeshCompletion {
  return {
    terminalResult: record.terminalResult,
    failureErrno: record.failureErrno,
    operationKind: record.operationKind,
    kindData: record.kindData,
    parts: record.parts.map(retainCompletionPart)
  };
}

function retainCompletionPart(part: ZLinkMessage): Message {
  // A runtime message already owns a managed Buffer and close() is a no-op.
  // Binding-native messages can expose native storage through data(), which
  // becomes invalid when their owner closes; materialize that one boundary copy.
  return part instanceof Message ? part : Message.fromOwned(Buffer.from(part.data()));
}

export function closeMeshCompletion(completion: ZLinkMeshCompletion): void {
  closeParts(completion.parts);
}

function closeParts(parts: readonly Message[]): void {
  for (const part of parts) {
    part.close();
  }
}
