import type { Message } from '../../contracts/Common/Message';
import type { ZLinkFrameworkRegistration } from '../configuration';
import { ZLinkConfigurationException } from '../configuration';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import { createAbortError, throwIfAborted } from '../abort';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  closeMessages,
  decodeChannelReply,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';
import { ZLinkSpotRouteTargetResolver } from './spot-route-target-resolver';

export class ZLinkSpotNodeRouteTransport {
  private readonly routerQueues = new Map<string, RouterOperationQueue>();

  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly targets: ZLinkSpotRouteTargetResolver
  ) {}

  async send(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    signal?: AbortSignal
  ): Promise<boolean> {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return false;
    }
    try {
      await this.enqueue(target.routerChannelId, () => {
        throwIfAborted(signal);
        if (!router.sendToSpot(target.targetNodeRid, target.spotId, parts, 0)) {
          throw this.notReady(target.routerChannelId, 'send');
        }
      });
      return true;
    } finally {
      closeMessages(parts);
    }
  }

  request<TReply>(
    target: ZLinkSpotRouteTarget,
    parts: readonly Message[],
    codecs: ZLinkChannelEnvelopeCodecRegistry | undefined,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply> | undefined {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return undefined;
    }
    const effectiveTimeoutMs = timeoutMs ?? this.registration.requestTimeoutMs ?? 30_000;
    return this.enqueuePhysicalRequest(target.routerChannelId, (releasePhysical) => {
      let physicalSubmitted = false;
      const result = this.submitRequest<TReply>(
        effectiveTimeoutMs,
        (resolve, reject) => {
          const submitted = router.requestToSpot(
            target.targetNodeRid,
            target.spotId,
            parts,
            (result, replyParts) => {
              releasePhysical();
              try {
                if (result !== 0) {
                  reject(this.requestFailure(target.routerChannelId, result));
                  return;
                }
                resolve(decodeChannelReply<TReply>(replyParts as readonly Message[], codecs));
              } catch (error) {
                reject(error);
              } finally {
                closeMessages(replyParts as readonly Message[]);
              }
            },
            0,
            effectiveTimeoutMs
          );
          physicalSubmitted = submitted;
          return submitted;
        },
        this.notReady(target.routerChannelId, 'request'),
        signal
      ).finally(() => closeMessages(parts));
      void result.catch(() => { if (!physicalSubmitted) releasePhysical(); });
      return result;
    });
  }

  requestRaw(
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]> | undefined {
    const router = this.targets.spotNodeRouter(target.routerChannelId);
    if (router === undefined) {
      return undefined;
    }
    const effectiveTimeoutMs = timeoutMs ?? this.registration.requestTimeoutMs ?? 30_000;
    return this.enqueuePhysicalRequest(target.routerChannelId, (releasePhysical) => {
      let physicalSubmitted = false;
      const result = this.submitRequest<readonly Message[]>(
        effectiveTimeoutMs,
        (resolve, reject) => {
          const submitted = router.requestToSpot(
            target.targetNodeRid,
            target.spotId,
            request,
            (result, replyParts) => {
              releasePhysical();
              if (signal?.aborted === true) {
                closeMessages(replyParts as readonly Message[]);
                reject(createAbortError());
                return;
              }
              if (result !== 0) {
                closeMessages(replyParts as readonly Message[]);
                reject(this.requestFailure(target.routerChannelId, result));
                return;
              }
              if (!resolve(replyParts as readonly Message[])) {
                closeMessages(replyParts as readonly Message[]);
              }
            },
            0,
            effectiveTimeoutMs
          );
          physicalSubmitted = submitted;
          return submitted;
        },
        this.notReady(target.routerChannelId, 'request'),
        signal
      );
      void result.catch(() => { if (!physicalSubmitted) releasePhysical(); });
      return result;
    });
  }

  private enqueue<T>(routerChannelId: string, operation: () => Promise<T> | T): Promise<T> {
    return this.queueFor(routerChannelId).enqueue(operation);
  }

  private enqueuePhysicalRequest<T>(
    routerChannelId: string,
    operation: (releasePhysical: () => void) => Promise<T>
  ): Promise<T> {
    return this.queueFor(routerChannelId).enqueuePhysical(operation);
  }

  private queueFor(routerChannelId: string): RouterOperationQueue {
    const existing = this.routerQueues.get(routerChannelId);
    if (existing !== undefined) return existing;
    let queue!: RouterOperationQueue;
    queue = new RouterOperationQueue(() => {
      if (this.routerQueues.get(routerChannelId) === queue) {
        this.routerQueues.delete(routerChannelId);
      }
    });
    this.routerQueues.set(routerChannelId, queue);
    return queue;
  }

  private submitRequest<T>(
    timeoutMs: number | undefined,
    submit: (resolve: (reply: T) => boolean, reject: (error: unknown) => void) => boolean,
    notReadyError: ZLinkConfigurationException,
    signal?: AbortSignal
  ): Promise<T> {
    throwIfAborted(signal);
    const effectiveTimeoutMs = timeoutMs ?? this.registration.requestTimeoutMs ?? 30_000;
    const deadline = Date.now() + effectiveTimeoutMs;
    return new Promise<T>((resolve, reject) => {
      let retryTimer: ReturnType<typeof setTimeout> | undefined;
      const deadlineTimer = setTimeout(() => fail(notReadyError), effectiveTimeoutMs);
      let settled = false;
      const cleanup = () => {
        clearTimeout(deadlineTimer);
        if (retryTimer !== undefined) {
          clearTimeout(retryTimer);
        }
        signal?.removeEventListener('abort', onAbort);
      };
      const complete = (value: T): boolean => {
        if (settled) return false;
        settled = true;
        cleanup();
        resolve(value);
        return true;
      };
      const fail = (error: unknown) => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(error);
      };
      const onAbort = () => fail(createAbortError());
      const attempt = () => {
        if (settled) return;
        try {
          if (submit(complete, fail)) {
            return;
          }
        } catch (error) {
          fail(error);
          return;
        }
        if (Date.now() >= deadline) {
          fail(notReadyError);
          return;
        }
        retryTimer = setTimeout(attempt, 10);
      };
      signal?.addEventListener('abort', onAbort, { once: true });
      attempt();
    });
  }

  private requestFailure(routerChannelId: string, result: number): Error {
    return createInternalFrameworkException(
      result === 102
        ? ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
        : ZLinkFrameworkInternalErrorKind.RouteNotConnected,
      `SpotNode router '${routerChannelId}' spot request failed with result ${result}.`,
      result === 109 || result === 113
    );
  }

  private notReady(routerChannelId: string, operation: 'request' | 'send'): ZLinkConfigurationException {
    return new ZLinkConfigurationException(
      `SpotNode router '${routerChannelId}' is not ready for SPOT ${operation}.`
    );
  }
}

interface RouterOperation<T> {
  readonly physical: boolean;
  readonly run: (releasePhysical: () => void) => Promise<T> | T;
  readonly resolve: (value: T | PromiseLike<T>) => void;
  readonly reject: (error: unknown) => void;
}

/** Serializes one native request slot without linking a tail Promise chain per queued operation. */
class RouterOperationQueue {
  private readonly operations: RouterOperation<unknown>[] = [];
  private head = 0;
  private running = false;

  constructor(private readonly onIdle: () => void) {}

  enqueue<T>(run: () => Promise<T> | T): Promise<T> {
    return this.add({
      physical: false,
      run: () => run()
    });
  }

  enqueuePhysical<T>(run: (releasePhysical: () => void) => Promise<T>): Promise<T> {
    return this.add({
      physical: true,
      run
    });
  }

  private add<T>(operation: {
    readonly physical: boolean;
    readonly run: (releasePhysical: () => void) => Promise<T> | T;
  }): Promise<T> {
    if (this.head > 64 && this.head * 2 >= this.operations.length) {
      this.operations.splice(0, this.head);
      this.head = 0;
    }
    const result = new Promise<T>((resolve, reject) => {
      this.operations.push({
        physical: operation.physical,
        run: operation.run,
        resolve: value => resolve(value as T),
        reject
      });
    });
    this.pump();
    return result;
  }

  private pump(): void {
    if (this.running) return;
    if (this.head >= this.operations.length) {
      this.operations.length = 0;
      this.head = 0;
      this.onIdle();
      return;
    }
    const operation = this.operations[this.head++];
    this.running = true;
    let released = false;
    const releasePhysical = () => {
      if (!operation.physical || released) return;
      released = true;
      // Native callbacks may be delivered synchronously by a test double. Defer
      // the next route operation so the current callback can finish first.
      queueMicrotask(() => this.finish());
    };
    let value: Promise<unknown> | unknown;
    try {
      value = operation.run(releasePhysical);
    } catch (error) {
      operation.reject(error);
      this.finish();
      return;
    }
    Promise.resolve(value).then(operation.resolve, operation.reject).finally(() => {
      // A physical request remains the queue owner after caller abort/timeout;
      // its native callback invokes releasePhysical when the late reply arrives.
      if (!operation.physical) this.finish();
    });
  }

  private finish(): void {
    if (!this.running) return;
    this.running = false;
    this.pump();
  }
}
