// SPDX-License-Identifier: MPL-2.0

export type ZLinkReceiveTaskErrorReporter = (error: unknown) => void;

/**
 * Tracks asynchronous receive dispatches without leaving rejected promises
 * detached from the runtime. Queue admission is owned by the host-wide
 * Application Job Queue; this tracker does not impose another capacity limit.
 */
export class ZLinkReceiveTaskTracker {
  private readonly tasks = new Set<Promise<void>>();

  constructor(private readonly reportError?: ZLinkReceiveTaskErrorReporter) {}

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

  async waitForAll(): Promise<void> {
    await Promise.allSettled([...this.tasks]);
  }

  private complete(task: Promise<void>): void {
    this.tasks.delete(task);
  }
}
