// SPDX-License-Identifier: MPL-2.0

export type ZLinkReceiveTaskErrorReporter = (error: unknown) => void;
export const ZLINK_MAX_CONCURRENT_CHANNEL_DISPATCHES = 1_024;

/**
 * Tracks asynchronous receive dispatches without leaving rejected promises
 * detached from the runtime. The bounded mode is an internal scheduler guard;
 * Application HWM continues to account for payload bytes only.
 */
export class ZLinkReceiveTaskTracker {
  private readonly tasks = new Set<Promise<void>>();
  private capacityWaiter?: Promise<void>;
  private wakeCapacity?: () => void;

  constructor(
    private readonly maxConcurrent = Number.POSITIVE_INFINITY,
    private readonly reportError?: ZLinkReceiveTaskErrorReporter
  ) {}

  track(task: Promise<void>, consumeError = true): void {
    this.tasks.add(task);
    void task.then(
      () => this.complete(task),
      error => {
        this.complete(task);
        if (consumeError) {
          try {
            this.reportError?.(error);
          } catch {
            // Error reporting must not create another unhandled rejection.
          }
        }
      }
    );
  }

  delete(task: Promise<void>): void {
    this.complete(task);
  }

  waitForCapacity(
    signal: AbortSignal | undefined,
    stopped: () => boolean
  ): Promise<void> | undefined {
    if (this.tasks.size < this.maxConcurrent) {
      return undefined;
    }
    return this.waitForCapacityLoop(signal, stopped);
  }

  private async waitForCapacityLoop(
    signal: AbortSignal | undefined,
    stopped: () => boolean
  ): Promise<void> {
    while (this.tasks.size >= this.maxConcurrent) {
      if (stopped() || signal?.aborted === true) return;
      const wake = this.capacityWaiter ?? this.createCapacityWaiter();
      if (signal === undefined) {
        await wake;
      } else {
        await waitForCapacityWakeOrAbort(wake, signal);
      }
    }
  }

  wakeCapacityWaiter(): void {
    this.wakeCapacity?.();
  }

  async waitForAll(): Promise<void> {
    await Promise.allSettled([...this.tasks]);
  }

  private createCapacityWaiter(): Promise<void> {
    const waiter = new Promise<void>((resolve) => {
      this.wakeCapacity = resolve;
    });
    this.capacityWaiter = waiter;
    return waiter;
  }

  private complete(task: Promise<void>): void {
    if (!this.tasks.delete(task)) return;
    this.capacityWaiter = undefined;
    const wake = this.wakeCapacity;
    this.wakeCapacity = undefined;
    wake?.();
  }
}

function waitForCapacityWakeOrAbort(
  wake: Promise<void>,
  signal: AbortSignal
): Promise<void> {
  if (signal.aborted) return Promise.resolve();
  return new Promise<void>((resolve) => {
    const onAbort = (): void => {
      signal.removeEventListener('abort', onAbort);
      resolve();
    };
    signal.addEventListener('abort', onAbort, { once: true });
    void wake.then(() => {
      signal.removeEventListener('abort', onAbort);
      resolve();
    });
  });
}
