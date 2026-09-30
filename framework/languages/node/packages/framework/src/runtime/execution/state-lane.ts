import { AsyncLocalStorage } from 'node:async_hooks';

type ZLinkStateLaneWork<T> = () => Promise<T> | T;

interface ZLinkStateLaneTurn {
  readonly lane: ZLinkStateLane;
  active: boolean;
}

const currentLaneStorage = new AsyncLocalStorage<ZLinkStateLane>();
const diagnosticPending =
  process.env.ZLINK_NODE_STRUCTURAL_GUARD === '1' || process.env.NODE_ENV === 'test'
    ? new Map<number, string>()
    : undefined;
let nextDiagnosticWork = 0;
const diagnosticTurnStorage =
  diagnosticPending === undefined ? undefined : new AsyncLocalStorage<ZLinkStateLaneTurn>();

/**
 * Single-owner execution lane for a component's mutable state.
 *
 * State owned by a lane is accessed only through its turns, so its collections
 * remain ordinary JavaScript collections. This is separate from the Spot and
 * Actor serial executors, which also own lifecycle and relocation concerns.
 */
export class ZLinkStateLane {
  private readonly mailbox: Array<() => Promise<void>> = [];
  private draining = false;
  private drainPromise: Promise<void> | undefined;
  private closed = false;

  static get current(): ZLinkStateLane | undefined {
    return currentLaneStorage.getStore();
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
        "This code already runs on the state lane it is trying to enter. Call the component's " +
          'private state method directly instead of re-entering its public surface.'
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
          await currentLaneStorage.run(this, work);
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

// Install the structural check only in diagnostic processes. The release run()
// method and its returned Promise are left untouched.
if (process.env.ZLINK_NODE_STRUCTURAL_GUARD === '1' || process.env.NODE_ENV === 'test') {
  Object.defineProperty(ZLinkStateLane, 'current', {
    configurable: true,
    get: () => {
      const turn = diagnosticTurnStorage?.getStore();
      return turn?.active === true ? turn.lane : undefined;
    }
  });

  const run = ZLinkStateLane.prototype.run;
  ZLinkStateLane.prototype.run = function <T>(
    this: ZLinkStateLane,
    work: ZLinkStateLaneWork<T>
  ): Promise<T> {
    const completion = run.call(this, () => runDiagnosticTurn(this, work)) as Promise<T>;
    return guardStateLaneCompletion(
      trackDiagnosticCompletion(completion, `state lane ${diagnosticWorkName(work)}`),
      'another state lane completion',
      this
    );
  };

  const tryPost = ZLinkStateLane.prototype.tryPost;
  ZLinkStateLane.prototype.tryPost = function (
    this: ZLinkStateLane,
    work: ZLinkStateLaneWork<void>
  ): boolean {
    const release = registerDiagnosticWork(`state lane post ${diagnosticWorkName(work)}`);
    const accepted = tryPost.call(this, () => {
      try {
        const result = runDiagnosticTurn(this, work);
        if (result instanceof Promise) return result.finally(release);
        release();
        return result;
      } catch (error) {
        release();
        throw error;
      }
    });
    if (!accepted) release();
    return accepted;
  };
}

function runDiagnosticTurn<T>(lane: ZLinkStateLane, work: ZLinkStateLaneWork<T>): Promise<T> | T {
  const turn: ZLinkStateLaneTurn = { lane, active: true };
  return diagnosticTurnStorage!.run(turn, () => {
    try {
      const result = work();
      if (result instanceof Promise) {
        return result.finally(() => {
          turn.active = false;
        });
      }
      turn.active = false;
      return result;
    } catch (error) {
      turn.active = false;
      throw error;
    }
  });
}

/** Applied only by debug-installed wrappers; release completions keep their original identity. */
export function guardStateLaneCompletion<T>(
  completion: Promise<T>,
  label: string,
  owner?: ZLinkStateLane
): Promise<T> {
  return new Proxy(completion, {
    get(target, property) {
      if (property === 'then') {
        return (onFulfilled: (value: T) => unknown, onRejected: (error: unknown) => unknown) => {
          const current = ZLinkStateLane.current;
          if (current !== undefined && current !== owner) {
            throw new Error(`A state lane turn cannot wait for ${label}.`);
          }
          return target.then(onFulfilled, onRejected);
        };
      }
      if (property === 'catch') {
        return (onRejected: (error: unknown) => unknown) =>
          guardStateLaneCompletion(target.catch(onRejected), label, owner);
      }
      if (property === 'finally') {
        return (onFinally: () => void) =>
          guardStateLaneCompletion(target.finally(onFinally), label, owner);
      }
      const value = Reflect.get(target, property, target) as unknown;
      return typeof value === 'function' ? value.bind(target) : value;
    }
  });
}

export function trackDiagnosticCompletion<T>(completion: Promise<T>, name: string): Promise<T> {
  const release = registerDiagnosticWork(name);
  void completion.then(release, release);
  return completion;
}

export function debugPendingWorkNames(): string[] {
  return [...(diagnosticPending?.values() ?? [])];
}

export function diagnosticWorkName(work: { readonly name: string }): string {
  return work.name.length > 0 ? work.name : '<anonymous>';
}

function registerDiagnosticWork(name: string): () => void {
  if (diagnosticPending === undefined) return () => undefined;
  const id = ++nextDiagnosticWork;
  const frames = new Error().stack?.split('\n').slice(2) ?? [];
  const caller =
    frames
      .find((frame) => !frame.includes('state-lane.') && !frame.includes('serial-execution-queue.'))
      ?.trim() ?? '<unknown caller>';
  diagnosticPending.set(id, `${name} ${caller}`);
  return () => {
    diagnosticPending.delete(id);
  };
}
