export interface OperationClock {
  now(): number;
}

export class OperationTimeoutError extends Error {
  constructor(readonly operationId: bigint) {
    super(`Operation ${operationId} timed out.`);
    this.name = 'OperationTimeoutError';
  }
}

export class OperationCancelledError extends Error {
  constructor(
    readonly operationId: bigint,
    message = 'Operation was cancelled.'
  ) {
    super(message);
    this.name = 'OperationCancelledError';
  }
}

export interface PendingOperation<T> {
  readonly id: bigint;
  readonly promise: Promise<T>;
}

interface Entry<T, Context> {
  readonly context: Context | undefined;
  readonly deadlineMs?: number;
  readonly resolve: (value: T | PromiseLike<T>) => void;
  readonly reject: (reason: unknown) => void;
}

const systemClock: OperationClock = {
  now: () => performance.now()
};

/** Owns request completion so reply, timeout, cancellation, and shutdown race safely. */
export class OperationRegistry<T, Context = unknown> {
  private readonly entries = new Map<bigint, Entry<T, Context>>();
  private nextId = 1n;
  private deadlineCursor?: MapIterator<[bigint, Entry<T, Context>]>;
  private closed = false;

  constructor(
    private readonly clock: OperationClock = systemClock,
    private readonly onPendingChanged?: () => void
  ) {}

  register(
    timeoutMs: number,
    timeoutOwner: 'registry' | 'sender' = 'registry',
    context?: Context
  ): PendingOperation<T> {
    if (this.closed) throw new Error('Operation registry is closed.');
    if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
      throw new RangeError('timeoutMs must be a non-negative finite number.');
    }
    const id = this.nextId++;
    let resolve!: Entry<T, Context>['resolve'];
    let reject!: Entry<T, Context>['reject'];
    const promise = new Promise<T>((onResolve, onReject) => {
      resolve = onResolve;
      reject = onReject;
    });
    const entry: Entry<T, Context> = {
      context,
      deadlineMs: timeoutOwner === 'registry' ? this.clock.now() + timeoutMs : undefined,
      resolve,
      reject
    };
    this.entries.set(id, entry);
    if (this.entries.size === 1) this.onPendingChanged?.();
    return { id, promise };
  }

  complete(id: bigint, value: T): boolean {
    const entry = this.take(id);
    if (entry === undefined) return false;
    entry.resolve(value);
    return true;
  }

  fail(id: bigint, reason: unknown): boolean {
    const entry = this.take(id);
    if (entry === undefined) return false;
    entry.reject(reason);
    return true;
  }

  cancel(id: bigint, message?: string): boolean {
    return this.fail(id, new OperationCancelledError(id, message));
  }

  isPending(id: bigint): boolean {
    return this.entries.has(id);
  }

  context(id: bigint): Context | undefined {
    return this.entries.get(id)?.context;
  }

  close(reason = 'Operation registry closed.'): void {
    if (this.closed) return;
    this.closed = true;
    for (const id of this.entries.keys()) this.cancel(id, reason);
    this.deadlineCursor = undefined;
  }

  get size(): number {
    return this.entries.size;
  }

  expire(nowMs: number, turnDeadlineMs: number): number {
    let expired = 0;
    while (this.clock.now() < turnDeadlineMs) {
      this.deadlineCursor ??= this.entries.entries();
      const current = this.deadlineCursor.next();
      if (current.done === true) {
        this.deadlineCursor = undefined;
        break;
      }
      const [id, entry] = current.value;
      if (entry.deadlineMs !== undefined && entry.deadlineMs <= nowMs) {
        if (this.fail(id, new OperationTimeoutError(id))) expired++;
      }
    }
    return expired;
  }

  private take(id: bigint): Entry<T, Context> | undefined {
    const entry = this.entries.get(id);
    if (entry === undefined) return undefined;
    this.entries.delete(id);
    if (this.entries.size === 0) this.onPendingChanged?.();
    return entry;
  }
}
