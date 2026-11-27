import { UINT64_MAX } from '@zlink-systems/stream-wire';
import {
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamMessage,
  ZlinkStreamMetadata,
  ZlinkStreamCodec
} from '../Contracts';
import type { ZlinkStreamWriteOptions } from './ZlinkStreamFrameSender';
import { decodeAdmissionQueue, type AdmissionPosition } from './Protocol/ZlinkStreamAdmissionQueue';
import { connectorError } from './ZlinkStreamSupport';

type Reply = ZlinkStreamMessage<ZlinkStreamEncodedPayload>;

interface PendingRequestOptions {
  readonly packetName: string;
  readonly timeoutMs: number;
  readonly maxAdmissionWaitMs?: number;
  readonly signal?: AbortSignal;
  send(requestSeq: bigint, writeOptions: ZlinkStreamWriteOptions, ticket?: string): Promise<void>;
  onAdmissionWaiting(retryAfterMs: number): void;
}

interface PendingRequest {
  resolve(value: () => ZlinkStreamEncodedPayload, metadata: ZlinkStreamMetadata): void;
  reject(error: unknown): void;
  waitForAdmission(retryAfterMs: number, receivedAt: number): void;
  waitForQueue(receivedAt: number): void;
  scheduleQueueWake(): void;
  retryWithTicket(ticket: string): void;
  expireIfDue(now: number): boolean;
}

const MAX_TIMER_DELAY_MS = 2_147_483_647;
const QUEUE_SILENCE_MS = 15_000;

interface ConnectionAdmissionQueue {
  readonly waiting: Set<PendingRequest>;
  wakeAt: number;
  attempt?: {
    readonly seq: bigint;
    readonly request: PendingRequest;
    readonly lifetime: AbortController;
  };
}

export class ZlinkStreamPendingRequests {
  private nextRequestSeq = 1n;
  private readonly active = new Set<PendingRequest>();
  private readonly attempts = new Map<bigint, PendingRequest>();
  private queue: ConnectionAdmissionQueue | undefined;

  constructor(
    private readonly sendEnqueue: (
      requestSeq: bigint,
      options: ZlinkStreamWriteOptions
    ) => Promise<void>,
    private readonly onQueued: () => void,
    private readonly onPosition: (position: AdmissionPosition) => void
  ) {}

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
    let ticket: string | undefined;
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
      this.removeQueueWaiter(pending);
      signal?.removeEventListener('abort', cancel);
      writeLifetime.abort();
      return true;
    };
    const cancel = (): void =>
      pending.reject(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));

    const schedule = (retryAt = Infinity): void => {
      clearTimeout(timer);
      const queue = this.queue;
      const watchesQueue = queue !== undefined && queue.waiting.values().next().value === pending;
      const wakeAt = Math.min(deadline, retryAt, watchesQueue ? queue.wakeAt : Infinity);
      timer = setTimeout(
        () => {
          const now = performance.now();
          if (pending.expireIfDue(now)) return;
          if (watchesQueue && now >= queue.wakeAt) this.enqueue(queue, now);
          else if (now >= retryAt) sendAttempt(true);
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
        .send(
          requestSeq,
          {
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
          },
          ticket
        )
        .catch(pending.reject);
    };
    const beginAdmissionWait = (receivedAt: number): void => {
      removeAttempt();
      firstRejectionAt ??= receivedAt;
      deadline = Math.min(deadline, firstRejectionAt + (options.maxAdmissionWaitMs ?? timeoutMs));
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
        beginAdmissionWait(receivedAt);
        schedule(receivedAt + retryAfterMs + Math.floor(Math.random() * 51));
        options.onAdmissionWaiting(retryAfterMs);
      },
      waitForQueue: (receivedAt) => {
        beginAdmissionWait(receivedAt);
        ticket = undefined;
        this.waitInQueue(pending, receivedAt);
      },
      scheduleQueueWake: () => schedule(),
      retryWithTicket: (issued) => {
        if (pending.expireIfDue(performance.now())) return;
        ticket = issued;
        sendAttempt(true);
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
    receivedAt = performance.now(),
    queueRequired?: string
  ): boolean {
    const queue = this.queue;
    if (queue?.attempt?.seq === requestSeq) {
      const request = queue.attempt.request;
      this.clearEnqueue(queue);
      if (!request.expireIfDue(receivedAt)) {
        request.reject(connectorError(error.code, error.message, error.cause));
      }
      return true;
    }
    const pending = this.attempts.get(requestSeq);
    if (pending === undefined) return false;
    if (pending.expireIfDue(receivedAt)) return true;

    if (queueRequired !== undefined) {
      if (queueRequired === '1' && retryAfter === undefined) {
        pending.waitForQueue(receivedAt);
      } else {
        pending.reject(
          connectorError(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            'Invalid admission queue metadata.'
          )
        );
      }
    } else if (retryAfter === undefined) {
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
    if (this.queue !== undefined) {
      const queue = this.queue;
      this.queue = undefined;
      this.clearEnqueue(queue);
    }
    for (const pending of this.active) {
      pending.reject(connectorError(error.code, error.message, error.cause));
    }
  }

  resolveEnqueue(seq: bigint, codec: ZlinkStreamCodec, payload: () => Uint8Array): boolean {
    const queue = this.queue;
    if (queue?.attempt?.seq !== seq) return false;
    const request = queue.attempt.request;
    this.clearEnqueue(queue);
    if (!this.hasQueueWaiters(queue, performance.now())) return true;
    try {
      this.acceptQueuePayload(queue, decodeAdmissionQueue(codec, payload), false);
    } catch (error) {
      request.reject(error);
    }
    return true;
  }

  receiveQueuePush(name: string, codec: ZlinkStreamCodec, payload: () => Uint8Array): void {
    const queue = this.queue;
    if (queue === undefined) return;
    if (!this.hasQueueWaiters(queue, performance.now())) return;
    let value: ReturnType<typeof decodeAdmissionQueue>;
    try {
      value = decodeAdmissionQueue(codec, payload, name);
    } catch (error) {
      // A Send has no request sequence. Fail the entire waiting group before closing.
      for (const request of queue.waiting) request.reject(error);
      throw error;
    }
    this.acceptQueuePayload(queue, value, true);
  }

  private waitInQueue(request: PendingRequest, now: number): void {
    if (this.queue !== undefined) {
      this.queue.waiting.add(request);
      request.scheduleQueueWake();
      return;
    }
    const queue: ConnectionAdmissionQueue = { waiting: new Set([request]), wakeAt: now };
    this.queue = queue;
    this.onQueued();
    this.enqueue(queue, now);
  }

  private enqueue(queue: ConnectionAdmissionQueue, now: number): void {
    const request = queue.waiting.values().next().value;
    if (request === undefined || this.queue !== queue) return;
    this.clearEnqueue(queue);
    if (this.nextRequestSeq > UINT64_MAX) {
      const error = connectorError(
        ZlinkStreamErrorCode.SendFailed,
        'Request sequence is exhausted.'
      );
      for (const waiting of queue.waiting) waiting.reject(error);
      return;
    }
    const seq = this.nextRequestSeq++;
    const lifetime = new AbortController();
    queue.attempt = { seq, request, lifetime };
    queue.wakeAt = now + QUEUE_SILENCE_MS;
    request.scheduleQueueWake();
    void this.sendEnqueue(seq, {
      signal: lifetime.signal,
      beforeWrite: () => {
        if (!this.hasQueueWaiters(queue, performance.now())) {
          throw connectorError(ZlinkStreamErrorCode.RequestTimeout, 'Admission queue wait ended.');
        }
      }
    }).catch((error: unknown) => {
      if (queue.attempt?.seq !== seq) return;
      this.clearEnqueue(queue);
      request.reject(error);
    });
  }

  private acceptQueuePayload(
    queue: ConnectionAdmissionQueue,
    value: ReturnType<typeof decodeAdmissionQueue>,
    push: boolean
  ): void {
    const now = performance.now();
    if ('ticket' in value) {
      this.queue = undefined;
      this.clearEnqueue(queue);
      for (const request of queue.waiting) request.retryWithTicket(value.ticket);
      return;
    }
    if (push) {
      queue.wakeAt = now + QUEUE_SILENCE_MS;
      queue.waiting.values().next().value?.scheduleQueueWake();
    }
    this.onPosition(value);
  }

  private hasQueueWaiters(queue: ConnectionAdmissionQueue, now: number): boolean {
    if (this.queue !== queue) return false;
    for (const request of queue.waiting) request.expireIfDue(now);
    return this.queue === queue;
  }

  private removeQueueWaiter(request: PendingRequest): void {
    const queue = this.queue;
    if (queue === undefined) return;
    const leader = queue.waiting.values().next().value;
    if (!queue.waiting.delete(request) || leader !== request) return;
    const next = queue.waiting.values().next().value;
    if (next === undefined) {
      this.queue = undefined;
      this.clearEnqueue(queue);
    } else {
      next.scheduleQueueWake();
    }
  }

  private clearEnqueue(queue: ConnectionAdmissionQueue): void {
    const attempt = queue.attempt;
    queue.attempt = undefined;
    attempt?.lifetime.abort();
  }
}
