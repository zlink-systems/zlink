import { AsyncLocalStorage } from 'node:async_hooks';

type ZLinkStateLaneWork<T> = () => Promise<T> | T;

/**
 * Single-owner execution lane for a component's mutable state.
 *
 * State owned by a lane is accessed only through its turns, so its collections
 * remain ordinary JavaScript collections. This is separate from the Spot and
 * Actor serial executors, which also own lifecycle and relocation concerns.
 */
export class ZLinkStateLane {
  private static readonly currentLaneStorage = new AsyncLocalStorage<ZLinkStateLane>();

  private readonly mailbox: Array<() => Promise<void>> = [];
  private draining = false;
  private drainPromise: Promise<void> | undefined;
  private closed = false;

  static get current(): ZLinkStateLane | undefined {
    return ZLinkStateLane.currentLaneStorage.getStore();
  }

  get isOnLane(): boolean {
    return ZLinkStateLane.current === this;
  }

  /** Runs work on the lane and resolves with its result. */
  run<T>(work: ZLinkStateLaneWork<T>): Promise<T> {
    this.throwIfReentrant();
    if (this.closed) {
      throw new Error('ZLink state lane is closed.');
    }

    let resolve!: (value: T) => void;
    let reject!: (reason: unknown) => void;
    const completion = new Promise<T>((complete, fail) => {
      resolve = complete;
      reject = fail;
    });
    this.mailbox.push(async () => {
      try {
        resolve(await work());
      } catch (error) {
        reject(error);
      }
    });
    this.scheduleDrain();
    return completion;
  }

  /** Queues work without waiting for its result. */
  tryPost(work: ZLinkStateLaneWork<void>): boolean {
    if (this.closed) {
      return false;
    }

    this.mailbox.push(async () => {
      await work();
    });
    this.scheduleDrain();
    return true;
  }

  /** Fails at the recursive call site instead of waiting for this lane's current turn. */
  throwIfReentrant(): void {
    if (this.isOnLane) {
      throw new Error(
        'This code already runs on the state lane it is trying to enter. Call the component\'s '
        + 'private state method directly instead of re-entering its public surface.'
      );
    }
  }

  /** Closes admission and waits for work already in the mailbox. */
  async dispose(): Promise<void> {
    if (this.closed) {
      return;
    }

    this.closed = true;
    this.scheduleDrain();
    await this.drainPromise;
  }

  private scheduleDrain(): void {
    if (this.draining) {
      return;
    }

    this.draining = true;
    this.drainPromise = this.drain();
  }

  private async drain(): Promise<void> {
    try {
      for (;;) {
        const work = this.mailbox.shift();
        if (work === undefined) {
          return;
        }
        try {
          await ZLinkStateLane.currentLaneStorage.run(this, work);
        } catch {
          // Posted callbacks own their errors. One failure must not strand later work.
        }
      }
    } finally {
      this.draining = false;
      this.drainPromise = undefined;
      if (this.mailbox.length > 0) {
        this.scheduleDrain();
      }
    }
  }
}
