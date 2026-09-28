import type { Disposable, ZlinkStreamConnectionStateChanged, ZlinkStreamError } from '../Contracts';
import { ZlinkStreamErrorCode } from '../Contracts';
import { subscription } from './ZlinkStreamSupport';

export class ZlinkStreamConnectorEvents {
  /**
   * @param enqueueCallback Queues a callback that calls the number of handlers
   *   `callbacks` reports for the handlers registered at that moment.
   */
  constructor(
    private readonly enqueueCallback: (callback: () => void, callbacks: () => number) => void
  ) {}
  private readonly errorHandlers = new Set<
    (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void
  >();
  private readonly disconnectedHandlers = new Set<(signal?: AbortSignal) => Promise<void> | void>();
  private readonly stateHandlers = new Set<
    (change: ZlinkStreamConnectionStateChanged, signal?: AbortSignal) => Promise<void> | void
  >();

  onError(
    handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    this.errorHandlers.add(handler);
    return subscription(() => this.errorHandlers.delete(handler));
  }

  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable {
    this.disconnectedHandlers.add(handler);
    return subscription(() => this.disconnectedHandlers.delete(handler));
  }

  onStateChanged(
    handler: (
      change: ZlinkStreamConnectionStateChanged,
      signal?: AbortSignal
    ) => Promise<void> | void
  ): Disposable {
    this.stateHandlers.add(handler);
    return subscription(() => this.stateHandlers.delete(handler));
  }
  publishError(error: ZlinkStreamError, signal?: AbortSignal): void {
    this.publish(this.errorHandlers, (handler) => handler(error, signal), signal);
  }

  publishDisconnected(signal?: AbortSignal): void {
    this.publish(this.disconnectedHandlers, (handler) => handler(signal), signal);
  }

  publishStateChanged(change: ZlinkStreamConnectionStateChanged, signal?: AbortSignal): void {
    this.publish(this.stateHandlers, (handler) => handler(change, signal), signal);
  }

  /**
   * Spec stream-connector 32 §7: the connector runs a registered callback and
   * does not wait for it to finish. A thrown or rejected failure reaches the
   * error handlers as `UserCallbackFailed`.
   */
  runUserCallback(
    run: () => Promise<void> | void,
    failureMessage: string,
    signal?: AbortSignal
  ): void {
    this.invoke(run, (cause) =>
      this.publishError(
        { code: ZlinkStreamErrorCode.UserCallbackFailed, message: failureMessage, cause },
        signal
      )
    );
  }

  private publish<T>(
    handlers: Set<T>,
    invoke: (handler: T) => Promise<void> | void,
    signal?: AbortSignal
  ): void {
    this.enqueueCallback(
      () => {
        for (const handler of Array.from(handlers)) {
          const report = (cause: unknown) =>
            this.reportFailure(
              cause,
              signal,
              handlers === this.errorHandlers ? handler : undefined
            );
          this.invoke(() => invoke(handler), report);
        }
      },
      () => handlers.size
    );
  }

  private reportFailure(cause: unknown, signal?: AbortSignal, failedHandler?: unknown): void {
    const error = {
      code: ZlinkStreamErrorCode.UserCallbackFailed,
      message: 'Connector event handler failed.',
      cause
    };
    if (failedHandler === undefined) {
      this.publishError(error, signal);
      return;
    }
    this.reportToRemaining(error, signal, new Set([failedHandler]));
  }

  private reportToRemaining(
    error: ZlinkStreamError,
    signal: AbortSignal | undefined,
    attempted: Set<unknown>
  ): void {
    const remaining = Array.from(this.errorHandlers).filter(
      (candidate) => !attempted.has(candidate)
    );
    if (remaining.length === 0) return;
    const nextAttempted = new Set([...attempted, ...remaining]);
    this.enqueueCallback(
      () => {
        for (const handler of remaining) {
          const report = (cause: unknown) =>
            this.reportToRemaining({ ...error, cause }, signal, nextAttempted);
          this.invoke(() => handler(error, signal), report);
        }
      },
      () => remaining.length
    );
  }

  private invoke(run: () => Promise<void> | void, report: (cause: unknown) => void): void {
    try {
      Promise.resolve(run()).catch(report);
    } catch (cause) {
      report(cause);
    }
  }
}
