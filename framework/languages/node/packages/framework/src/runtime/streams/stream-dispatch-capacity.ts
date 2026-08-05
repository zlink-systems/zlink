import type { ZLinkApplicationWorkClaim } from '../admission';

/**
 * Bounds framework work items whose payload size is zero and therefore cannot
 * be bounded by the application byte budget.
 */
export const ZLINK_STREAM_MAX_IN_FLIGHT_DISPATCHES = 1024;

export class ZLinkStreamDispatchCapacity {
  private active = 0;
  private readonly waiters: Array<(() => void) | undefined> = [];
  private waiterHead = 0;
  private waiterCount = 0;

  get receivePaused(): boolean {
    return this.active >= ZLINK_STREAM_MAX_IN_FLIGHT_DISPATCHES;
  }

  tryClaim(): ZLinkApplicationWorkClaim | undefined {
    if (this.receivePaused) {
      return undefined;
    }
    this.active += 1;
    let released = false;
    return {
      close: () => {
        if (released) {
          return;
        }
        released = true;
        this.active -= 1;
        if (this.waiterCount > 0 && !this.receivePaused) {
          this.takeWaiter()?.();
        }
      }
    };
  }

  waitUntilResumed(signal?: AbortSignal): Promise<void> {
    if (!this.receivePaused || signal?.aborted === true) {
      return Promise.resolve();
    }
    return new Promise<void>((resolve) => {
      let waiting = true;
      const finish = (): void => {
        if (!waiting) {
          return;
        }
        waiting = false;
        const index = this.waiters.indexOf(finish, this.waiterHead);
        if (index >= 0) {
          this.waiters[index] = undefined;
          this.waiterCount -= 1;
          this.advanceWaiterHead();
          this.compactWaiters();
        }
        signal?.removeEventListener('abort', finish);
        resolve();
      };
      this.waiters.push(finish);
      this.waiterCount += 1;
      signal?.addEventListener('abort', finish, { once: true });
      if (!this.receivePaused) {
        finish();
      }
    });
  }

  private takeWaiter(): (() => void) | undefined {
    this.advanceWaiterHead();
    const waiter = this.waiters[this.waiterHead];
    if (waiter === undefined) return undefined;
    this.waiters[this.waiterHead] = undefined;
    this.waiterHead += 1;
    this.waiterCount -= 1;
    this.compactWaiters();
    return waiter;
  }

  private advanceWaiterHead(): void {
    while (this.waiterHead < this.waiters.length && this.waiters[this.waiterHead] === undefined) {
      this.waiterHead += 1;
    }
  }

  private compactWaiters(): void {
    if (this.waiterCount === 0) {
      this.waiters.length = 0;
      this.waiterHead = 0;
    } else if (this.waiterHead >= 1024 && this.waiterHead * 2 >= this.waiters.length) {
      this.waiters.splice(0, this.waiterHead);
      this.waiterHead = 0;
    }
  }
}
