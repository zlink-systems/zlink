import { requestTerminalError } from '../messaging/request-deadline';
import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '../../contracts';
import {
  OperationRegistry,
  OperationCancelledError,
  type OperationHandle,
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
    existing?: PendingOperation<unknown>
  ): PendingOperation<T> {
    if (existing !== undefined) return existing as PendingOperation<T>;
    const operation = this.registry.register(
      timeoutMs ?? 0,
      timeoutMs === undefined ? 'sender' : 'registry'
    );
    operation.setSignal(signal);
    return operation as PendingOperation<T>;
  }

  run<T>(
    pending: PendingOperation<unknown>,
    work: () => Promise<T> | ZLinkRequestResult<T>,
    discardLateResult?: (value: T) => void
  ): Promise<T> {
    if (!pending.isPending()) return pending.promise as Promise<T>;
    pending.deferCompletion();
    try {
      const result = work();
      const prepared = 'pending' in result ? result : undefined;
      const source = prepared?.pending ?? (result as Promise<T>);
      void source.then(
        (value) => {
          let reply: T;
          try {
            reply = prepared === undefined ? (value as T) : prepared.decode(value);
          } catch (error) {
            pending.fail(requestTerminalError('Request', error));
            return;
          }
          if (!pending.complete(reply)) discardLateResult?.(reply);
        },
        (error) => {
          let failure = error;
          if (prepared !== undefined) {
            try {
              failure = prepared.mapFailure(error);
            } catch (mapped) {
              failure = mapped;
            }
          }
          pending.fail(requestTerminalError('Request', failure));
        }
      );
    } catch (error) {
      pending.fail(requestTerminalError('Request', error));
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
    return this.run(pending, () => work(pending), discardLateResult);
  }

  prepare<T>(
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    build: (pending: PendingOperation<unknown>) => ZLinkRequestResult<T>
  ): ZLinkRequestResult<T> {
    if (signal?.aborted === true) return rejectZLinkRequestReply(createAbortError());
    const operation = this.begin(timeoutMs, signal);
    const pending = this.run(operation, () => build(operation));
    return {
      pending,
      decode: identityRequestReply<T>,
      mapFailure: identityRequestReply,
      operation: operation as PendingOperation<T>
    };
  }

  observe<T, R = T>(
    timeoutMs: number,
    submit: (entry: OperationHandle<T>) => void,
    onReply: (value: T) => R,
    onFailed: (error: unknown) => R
  ): Promise<R> {
    return new Promise<R>((resolve, reject) => {
      const id = this.registry.registerObserved(timeoutMs, {
        operationCompleted: (_id, value) => {
          try {
            resolve(onReply(value as T));
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
      });
      const entry = this.registry.handle(id) as OperationHandle<T>;
      try {
        submit(entry);
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
