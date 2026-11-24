import { UINT64_MAX } from '@zlink-systems/stream-wire';
import {
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamMessage,
  ZlinkStreamMetadata
} from '../Contracts';
import type { ZlinkStreamWriteOptions } from './ZlinkStreamFrameSender';
import { connectorError } from './ZlinkStreamSupport';

type Reply = ZlinkStreamMessage<ZlinkStreamEncodedPayload>;

interface PendingRequestOptions {
  readonly packetName: string;
  readonly timeoutMs: number;
  readonly maxAdmissionWaitMs?: number;
  readonly signal?: AbortSignal;
  send(requestSeq: bigint, writeOptions: ZlinkStreamWriteOptions): Promise<void>;
  onAdmissionWaiting(retryAfterMs: number): void;
}

interface PendingRequest {
  resolve(value: () => ZlinkStreamEncodedPayload, metadata: ZlinkStreamMetadata): void;
  reject(error: unknown): void;
  waitForAdmission(retryAfterMs: number, receivedAt: number): void;
  expireIfDue(now: number): boolean;
}

const MAX_TIMER_DELAY_MS = 2_147_483_647;

export class ZlinkStreamPendingRequests {
  private nextRequestSeq = 1n;
  private readonly active = new Set<PendingRequest>();
  private readonly attempts = new Map<bigint, PendingRequest>();

  submit(options: PendingRequestOptions): Promise<Reply> {
    // Property reads, not destructuring: emscripten JSDCE deletes destructuring declarations.
    const packetName = options.packetName;
    const signal = options.signal;
    const timeoutMs = Number.isFinite(options.timeoutMs) ? Math.max(1, options.timeoutMs) : 1;
    const writeLifetime = new AbortController();
    let requestSeq: bigint | undefined;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let deadline = Infinity;
    let firstRejectionAt: number | undefined;
    let resolve!: (value: Reply) => void;
    let reject!: (error: unknown) => void;
    const promise = new Promise<Reply>((accept, fail) => {
      resolve = accept;
      reject = fail;
    });

    const timeoutError = () =>
      connectorError(ZlinkStreamErrorCode.RequestTimeout, `Request '${packetName}' timed out.`);
    const removeAttempt = (): void => {
      if (requestSeq === undefined) return;
      this.attempts.delete(requestSeq);
      requestSeq = undefined;
    };
    const finish = (): boolean => {
      if (!this.active.delete(pending)) return false;
      removeAttempt();
      clearTimeout(timer);
      signal?.removeEventListener('abort', cancel);
      writeLifetime.abort();
      return true;
    };
    const cancel = (): void =>
      pending.reject(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));

    const schedule = (retryAt = Infinity): void => {
      clearTimeout(timer);
      const wakeAt = Math.min(deadline, retryAt);
      timer = setTimeout(
        () => {
          const now = performance.now();
          if (pending.expireIfDue(now)) return;
          if (now >= retryAt) sendAttempt(true);
          else schedule(retryAt);
        },
        Math.min(Math.ceil(wakeAt - performance.now()), MAX_TIMER_DELAY_MS)
      );
    };
    const sendAttempt = (retry: boolean): void => {
      if (this.nextRequestSeq > UINT64_MAX) {
        pending.reject(
          connectorError(ZlinkStreamErrorCode.SendFailed, 'Request sequence is exhausted.')
        );
        return;
      }
      requestSeq = this.nextRequestSeq++;
      this.attempts.set(requestSeq, pending);
      if (retry) schedule();
      void options
        .send(requestSeq, {
          signal: writeLifetime.signal,
          onAccepted: retry
            ? undefined
            : () => {
                deadline = performance.now() + timeoutMs;
                schedule();
              },
          beforeWrite: retry
            ? () => {
                if (performance.now() >= deadline) throw timeoutError();
              }
            : undefined,
          onFailure: pending.reject
        })
        .catch(pending.reject);
    };
    const pending: PendingRequest = {
      resolve: (value, metadata) => {
        if (pending.expireIfDue(performance.now())) return;
        const payload = value();
        if (finish()) resolve({ name: packetName, metadata, payload });
      },
      reject: (error) => {
        if (finish()) reject(error);
      },
      expireIfDue: (now) => {
        if (now < deadline) return false;
        pending.reject(timeoutError());
        return true;
      },
      waitForAdmission: (retryAfterMs, receivedAt) => {
        removeAttempt();
        firstRejectionAt ??= receivedAt;
        deadline = Math.min(deadline, firstRejectionAt + (options.maxAdmissionWaitMs ?? timeoutMs));
        schedule(receivedAt + retryAfterMs + Math.floor(Math.random() * 51));
        options.onAdmissionWaiting(retryAfterMs);
      }
    };
    this.active.add(pending);
    signal?.addEventListener('abort', cancel, { once: true });
    if (signal?.aborted === true) cancel();
    else sendAttempt(false);
    return promise;
  }

  /* Stream connector spec §5.2 matches replies by the current request sequence. */
  resolve(
    requestSeq: bigint,
    value: () => ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata
  ): void {
    this.attempts.get(requestSeq)?.resolve(value, metadata);
  }

  reject(
    requestSeq: bigint,
    error: ZlinkStreamError,
    retryAfter?: string,
    receivedAt = performance.now()
  ): boolean {
    const pending = this.attempts.get(requestSeq);
    if (pending === undefined) return false;
    if (pending.expireIfDue(receivedAt)) return true;

    if (retryAfter === undefined) {
      pending.reject(connectorError(error.code, error.message, error.cause));
    } else {
      const retryAfterMs = Number(retryAfter);
      if (
        !Number.isInteger(retryAfterMs) ||
        retryAfterMs < 0 ||
        retryAfterMs > MAX_TIMER_DELAY_MS ||
        String(retryAfterMs) !== retryAfter
      ) {
        pending.reject(
          connectorError(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            'Invalid authentication admission retry delay.'
          )
        );
      } else {
        pending.waitForAdmission(retryAfterMs, receivedAt);
      }
    }
    return true;
  }

  failAll(error: ZlinkStreamError): void {
    for (const pending of this.active) {
      pending.reject(connectorError(error.code, error.message, error.cause));
    }
  }
}
