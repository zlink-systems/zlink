import { requestTerminalError } from '../messaging/request-deadline';
import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '../../contracts';
import {
  OperationRegistry,
  OperationCancelledError,
  type OperationHandle,
  type OperationObserver,
  type PendingOperation
} from './operation-registry';
import { createAbortError } from '../abort';
import {
  identityRequestReply,
  rejectZLinkRequestReply,
  type ZLinkRequestResult
} from '../messaging/request-result';

/** Owns the request entries and deadline visits of one runtime instance. */
export class RequestRuntime {
  readonly registry = new OperationRegistry<unknown, unknown>(
    undefined,
    () => this.schedule(),
    (reason) => requestTerminalError('Request', reason)
  );
  private timer?: NodeJS.Timeout;
  private dueMs = Number.POSITIVE_INFINITY;

  begin<T>(
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    existing?: PendingOperation<unknown>,
    absoluteDeadlineMs?: number
  ): PendingOperation<T> {
    if (existing !== undefined) return existing as PendingOperation<T>;
    const operation = this.registry.register(
      timeoutMs ?? 0,
      timeoutMs === undefined ? 'sender' : 'registry',
      undefined,
      absoluteDeadlineMs
    );
    operation.setSignal(signal);
    return operation as PendingOperation<T>;
  }

  run<T>(
    pending: PendingOperation<unknown>,
    start: (receiver: OperationObserver<unknown, unknown>) => void,
    decode: (raw: unknown) => T,
    mapFailure: (error: unknown) => unknown,
    destination?: OperationObserver<unknown, unknown>
  ): Promise<T> {
    if (!pending.isPending()) return pending.promise as Promise<T>;
    pending.deferCompletion();
    const receiver: OperationObserver<unknown, unknown> = {
      operationCompleted: (_id, raw) => {
        try {
          const value = decode(raw);
          if (destination !== undefined)
            destination.operationCompleted(pending.id, value, undefined);
          else pending.complete(value);
        } catch (error) {
          if (destination !== undefined) destination.operationFailed(pending.id, error, undefined);
          else pending.fail(error);
        }
      },
      operationFailed: (_id, error) => {
        let failure = error;
        try {
          failure = mapFailure(error);
        } catch (mapped) {
          failure = mapped;
        }
        if (destination !== undefined) destination.operationFailed(pending.id, failure, undefined);
        else pending.fail(failure);
      }
    };
    try {
      start(receiver);
    } catch (error) {
      receiver.operationFailed(pending.id, error, undefined);
    }
    return pending.promise as Promise<T>;
  }

  request<T>(
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    work: (pending: PendingOperation<unknown>) => Promise<T>,
    existing?: PendingOperation<unknown>,
    discardLateResult?: (value: T) => void
  ): Promise<T> {
    if (existing !== undefined) {
      if (!existing.isPending())
        return existing.promise.then<T>(() => {
          throw new OperationCancelledError(existing.id, 'Request is already terminal.');
        });
      return work(existing);
    }
    const pending = this.begin(timeoutMs, signal, existing);
    return this.run(
      pending,
      (receiver) => {
        void work(pending).then(
          (value) => receiver.operationCompleted(pending.id, value, undefined),
          (error) => receiver.operationFailed(pending.id, error, undefined)
        );
      },
      (value) => {
        if (!pending.isPending()) discardLateResult?.(value as T);
        return value as T;
      },
      identityRequestReply
    );
  }

  prepare<T>(
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    start: (
      pending: PendingOperation<unknown>,
      receiver: OperationObserver<unknown, unknown>
    ) => void,
    decode: (raw: unknown) => T,
    mapFailure: (error: unknown) => unknown,
    existing?: PendingOperation<unknown>,
    destination?: OperationObserver<unknown, unknown>
  ): ZLinkRequestResult<T> {
    if (signal?.aborted === true && destination === undefined)
      return rejectZLinkRequestReply(signal.reason ?? createAbortError());
    const operation = this.begin(timeoutMs, signal, existing);
    if (signal?.aborted === true && destination !== undefined)
      destination.operationFailed(operation.id, signal.reason ?? createAbortError(), undefined);
    const pending =
      signal?.aborted === true
        ? operation.promise
        : this.run(
            operation,
            (receiver) => start(operation, receiver),
            decode,
            mapFailure,
            destination
          );
    return {
      pending,
      decode: identityRequestReply<T>,
      mapFailure: identityRequestReply,
      operation: operation as PendingOperation<T>
    };
  }

  observe<T, R = T>(
    timeoutMs: number,
    submit: (entry: OperationHandle<T>, receiver: OperationObserver<T, unknown>) => void,
    onReply: (value: T) => R,
    onFailed: (error: unknown) => R
  ): Promise<R> {
    return new Promise<R>((resolve, reject) => {
      const observer: OperationObserver<T, unknown> = {
        operationCompleted: (_id, value) => {
          try {
            resolve(onReply(value));
          } catch (error) {
            reject(error);
          }
        },
        operationFailed: (_id, reason) => {
          try {
            resolve(onFailed(requestTerminalError('Request', reason)));
          } catch (error) {
            reject(error);
          }
        }
      };
      const id = this.registry.registerObserved(timeoutMs, observer);
      const entry = this.registry.handle(id) as OperationHandle<T>;
      try {
        submit(entry, {
          operationCompleted: (_id, value) => {
            if (!entry.complete(value)) observer.operationCompleted(id, value, undefined);
          },
          operationFailed: (_id, error) => entry.fail(error)
        });
      } catch (error) {
        entry.fail(error);
      }
    });
  }

  operations<T, Context = unknown>(): OperationRegistry<T, Context> {
    return this.registry as unknown as OperationRegistry<T, Context>;
  }

  close(
    reason: unknown = new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ShuttingDown,
      'Request runtime closed.'
    )
  ): void {
    this.registry.clear(reason);
    this.schedule();
  }

  private schedule(): void {
    const next = this.registry.nextDeadlineMs();
    if (next < this.dueMs) {
      clearTimeout(this.timer);
      this.timer = undefined;
      this.dueMs = next;
      if (Number.isFinite(next))
        this.timer = setTimeout(
          this.visit,
          Math.min(0x7fff_ffff, Math.ceil(next - performance.now()))
        );
    }
    if (this.timer !== undefined) {
      if (this.registry.size === 0) this.timer.unref();
      else this.timer.ref();
    }
  }

  private readonly visit = (): void => {
    this.timer = undefined;
    this.dueMs = Number.POSITIVE_INFINITY;
    const now = performance.now();
    this.registry.expire(now, now + 1);
    this.schedule();
  };
}
