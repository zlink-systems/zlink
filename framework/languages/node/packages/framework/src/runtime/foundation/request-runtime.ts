import { requestTerminalError } from '../messaging/request-deadline';
import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '../../contracts';
import {
  OperationRegistry,
  OperationCancelledError,
  type OperationHandle,
  type PendingOperation
} from './operation-registry';
import { createAbortError } from '../abort';

/** Owns the request entries and deadline visits of one runtime instance. */
export class RequestRuntime {
  readonly registry = new OperationRegistry<unknown, unknown>(undefined, () => this.schedule());
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
    const cancel = () => operation.fail(signal?.reason ?? createAbortError());
    const detach = () => signal?.removeEventListener('abort', cancel);
    void operation.promise.then(detach, detach);
    if (signal?.aborted === true) cancel();
    else signal?.addEventListener('abort', cancel, { once: true });
    return operation as PendingOperation<T>;
  }

  async run<T>(pending: PendingOperation<unknown>, work: () => Promise<T>): Promise<T> {
    try {
      if (!pending.isPending()) {
        await pending.promise;
        throw new OperationCancelledError(pending.id, 'Request is already terminal.');
      }
      const value = await work();
      pending.complete(value);
      return value;
    } catch (error) {
      const terminal = requestTerminalError('Request', error);
      pending.fail(terminal);
      throw terminal;
    }
  }

  request<T>(
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    work: (pending: PendingOperation<unknown>) => Promise<T>,
    existing?: PendingOperation<unknown>
  ): Promise<T> {
    const pending = this.begin(timeoutMs, signal, existing);
    return this.run(pending, () => work(pending));
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
