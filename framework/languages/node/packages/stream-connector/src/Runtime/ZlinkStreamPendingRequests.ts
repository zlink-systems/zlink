import {
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamMessage,
  ZlinkStreamMetadata
} from '../Contracts';
import { connectorError } from './ZlinkStreamSupport';

export interface PendingZlinkStreamRequest {
  readonly requestSeq: bigint;
  readonly promise: Promise<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>;
  startTimeout(): void;
}

interface TrackedPendingRequest {
  readonly packetName: string;
  readonly promise: Promise<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>;
  resolve(value: ZlinkStreamMessage<ZlinkStreamEncodedPayload>): void;
  reject(error: ZlinkStreamError): void;
  cancel(): void;
  startTimeout(): void;
}

export class ZlinkStreamPendingRequests {
  private nextRequestSeq = 1n;
  private readonly active = new Map<bigint, TrackedPendingRequest>();

  create(packetName: string, timeoutMs: number): PendingZlinkStreamRequest {
    if (this.nextRequestSeq > 0xffff_ffff_ffff_ffffn) {
      throw connectorError(ZlinkStreamErrorCode.SendFailed, 'Request sequence is exhausted.');
    }
    const requestSeq = this.nextRequestSeq++;
    let timeout: ReturnType<typeof setTimeout> | undefined;
    let resolvePending!: (value: ZlinkStreamMessage<ZlinkStreamEncodedPayload>) => void;
    let rejectPending!: (error: ZlinkStreamError) => void;
    const promise = new Promise<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>(
      (resolve, reject) => {
        resolvePending = resolve;
        rejectPending = (error) => reject(connectorError(error.code, error.message, error.cause));
      }
    );
    const tracked: TrackedPendingRequest = {
      packetName,
      promise,
      startTimeout: () => {
        if (timeout !== undefined || this.active.get(requestSeq) !== tracked) return;
        timeout = setTimeout(() => {
          this.active.delete(requestSeq);
          rejectPending({
            code: ZlinkStreamErrorCode.RequestTimeout,
            message: `Request '${packetName}' timed out.`
          });
        }, timeoutMs);
      },
      resolve: (value) => {
        if (timeout !== undefined) {
          clearTimeout(timeout);
        }
        resolvePending(value);
      },
      reject: (error) => {
        if (timeout !== undefined) {
          clearTimeout(timeout);
        }
        rejectPending(error);
      },
      cancel: () => {
        if (timeout !== undefined) {
          clearTimeout(timeout);
        }
      }
    };
    this.active.set(requestSeq, tracked);
    return { requestSeq, promise, startTimeout: tracked.startTimeout };
  }

  /* stream connector spec §5.2: a pending request is matched by request_seq alone. */
  resolve(
    requestSeq: bigint,
    value: () => ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata
  ): boolean {
    const pending = this.active.get(requestSeq);
    if (pending === undefined) {
      return false;
    }
    const payload = value();
    this.active.delete(requestSeq);
    pending.resolve({ name: pending.packetName, metadata, payload });
    return true;
  }

  reject(requestSeq: bigint, error: ZlinkStreamError): boolean {
    const pending = this.active.get(requestSeq);
    if (pending === undefined) {
      return false;
    }
    this.active.delete(requestSeq);
    pending.reject(error);
    return true;
  }

  cancel(requestSeq: bigint): void {
    const pending = this.active.get(requestSeq);
    if (pending === undefined) {
      return;
    }
    this.active.delete(requestSeq);
    pending.cancel();
  }

  failAll(error: ZlinkStreamError): void {
    for (const [requestSeq, pending] of this.active) {
      this.active.delete(requestSeq);
      pending.reject(error);
    }
  }
}
