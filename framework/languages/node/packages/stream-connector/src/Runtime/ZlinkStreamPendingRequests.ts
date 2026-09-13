import { ZlinkStreamEncodedPayload, ZlinkStreamError, ZlinkStreamErrorCode } from '../Contracts';
import { connectorError, throwIfAborted } from './ZlinkStreamSupport';

export interface PendingZlinkStreamRequest {
  readonly requestSeq: bigint;
  readonly promise: Promise<ZlinkStreamEncodedPayload>;
}

interface TrackedPendingRequest {
  readonly packetName: string;
  resolve(value: ZlinkStreamEncodedPayload): void;
  reject(error: ZlinkStreamError): void;
  dispose(): void;
}

export class ZlinkStreamPendingRequests {
  private nextRequestSeq = 1n;
  private readonly active = new Map<bigint, TrackedPendingRequest>();

  get count(): number {
    return this.active.size;
  }

  create(packetName: string, timeoutMs: number, signal?: AbortSignal): PendingZlinkStreamRequest {
    throwIfAborted(signal);
    const requestSeq = this.nextRequestSeq++;
    let timeout: ReturnType<typeof setTimeout> | undefined;
    let resolvePending!: (value: ZlinkStreamEncodedPayload) => void;
    let rejectPending!: (error: ZlinkStreamError) => void;
    const promise = new Promise<ZlinkStreamEncodedPayload>((resolve, reject) => {
      resolvePending = resolve;
      rejectPending = (error) => reject(connectorError(error.code, error.message, error.cause));
    });
    const onAbort = () => this.reject(requestSeq, {
      code: ZlinkStreamErrorCode.Disconnected,
      message: 'Operation canceled.',
      cause: signal?.reason
    });
    this.active.set(requestSeq, {
      packetName,
      resolve: resolvePending,
      reject: rejectPending,
      dispose: () => {
        if (timeout !== undefined) {
          clearTimeout(timeout);
        }
        signal?.removeEventListener('abort', onAbort);
      }
    });
    timeout = setTimeout(() => this.reject(requestSeq, {
      code: ZlinkStreamErrorCode.RequestTimeout,
      message: `Request '${packetName}' timed out.`
    }), timeoutMs);
    signal?.addEventListener('abort', onAbort, { once: true });
    return { requestSeq, promise };
  }

  /* stream connector spec §5.2: a pending request is matched by request_seq alone. Diagnostics
   * use the original request name retained by this registry, never a legacy reply name. */
  resolve(requestSeq: bigint, value: ZlinkStreamEncodedPayload): boolean {
    const pending = this.take(requestSeq);
    if (pending === undefined) {
      return false;
    }
    pending.resolve(value);
    return true;
  }

  reject(requestSeq: bigint, error: ZlinkStreamError): boolean {
    const pending = this.take(requestSeq);
    if (pending === undefined) {
      return false;
    }
    pending.reject(error);
    return true;
  }

  private take(requestSeq: bigint): TrackedPendingRequest | undefined {
    const pending = this.active.get(requestSeq);
    if (pending === undefined) {
      return undefined;
    }
    this.active.delete(requestSeq);
    pending.dispose();
    return pending;
  }

  failAll(error: ZlinkStreamError): void {
    for (const requestSeq of this.active.keys()) {
      this.reject(requestSeq, error);
    }
  }
}
