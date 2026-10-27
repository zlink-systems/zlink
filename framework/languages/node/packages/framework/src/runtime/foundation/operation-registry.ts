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

export interface OperationHandle<T> {
  readonly id: bigint;
  readonly deadlineMs: number | undefined;
  readonly promise?: Promise<T>;
  complete(value: T, retentionMs?: number): boolean;
  fail(reason: unknown): boolean;
  takeFailure(reason: unknown): boolean;
  isPending(): boolean;
  setFailureDispatcher(dispatcher: (reason: unknown) => void): void;
  clearFailureDispatcher(dispatcher: (reason: unknown) => void): void;
}

export interface PendingOperation<T> extends OperationHandle<T> {
  readonly promise: Promise<T>;
}

/**
 * Receives an operation's terminal at the place it is taken from the
 * registry. One long-lived observer serves every observed operation, so an
 * observed registration creates no Promise or per-operation callback.
 */
export interface OperationObserver<T, Context> {
  operationCompleted(id: bigint, value: T, context: Context | undefined): void;
  operationFailed(id: bigint, reason: unknown, context: Context | undefined): void;
}

interface Entry<T, Context> {
  context: Context | undefined;
  deadlineMs?: number;
  resolve?: (value: T | PromiseLike<T>) => void;
  reject?: (reason: unknown) => void;
  observer?: OperationObserver<T, Context>;
  failureDispatcher?: (reason: unknown) => void;
}

const DEADLINE_ROTATION_MS = 256;

const systemClock: OperationClock = {
  now: () => performance.now()
};

/** Owns request completion so reply, timeout, cancellation, and shutdown race safely. */
export class OperationRegistry<T, Context = unknown> {
  private readonly entries = new Map<bigint, Entry<T, Context>>();
  private nextId = 1n;
  private lastVisitMs: number;
  private deadlineCursor?: MapIterator<[bigint, Entry<T, Context>]>;
  /**
   * A lower bound of the earliest pending deadline. Completed entries only
   * leave it early; a full expiry pass replaces it with the exact value.
   */
  private earliestDeadlineMs = Number.POSITIVE_INFINITY;
  private scanEarliestDeadlineMs = Number.POSITIVE_INFINITY;
  private closed = false;
  private pendingCount = 0;

  constructor(
    private readonly clock: OperationClock = systemClock,
    private readonly onPendingChanged?: () => void
  ) {
    this.lastVisitMs = this.clock.now();
  }

  register(
    timeoutMs: number,
    timeoutOwner: 'registry' | 'sender' = 'registry',
    context?: Context,
    absoluteDeadlineMs?: number
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
    const deadlineMs =
      timeoutOwner === 'registry'
        ? (absoluteDeadlineMs ?? this.clock.now() + timeoutMs)
        : undefined;
    const entry: Entry<T, Context> = { context, deadlineMs, resolve, reject };
    this.entries.set(id, entry);
    this.notePending(deadlineMs);
    return {
      ...this.handle(id),
      promise,
      setFailureDispatcher: (dispatcher) => {
        if (!this.setFailureDispatcher(id, dispatcher)) void promise.then(undefined, dispatcher);
      }
    };
  }

  handle(id: bigint): OperationHandle<T> {
    return {
      id,
      deadlineMs: this.deadline(id),
      complete: (value, retentionMs) => this.complete(id, value, retentionMs),
      fail: (reason) => this.fail(id, reason),
      takeFailure: (reason) => this.takeFailure(id, reason),
      setFailureDispatcher: (dispatcher) => {
        this.setFailureDispatcher(id, dispatcher);
      },
      clearFailureDispatcher: (dispatcher) => this.clearFailureDispatcher(id, dispatcher),
      isPending: () => this.isPending(id)
    };
  }

  /** Registers an operation whose terminal goes to `observer` instead of a Promise. */
  registerObserved(
    timeoutMs: number,
    observer: OperationObserver<T, Context>,
    context?: Context
  ): bigint {
    if (this.closed) throw new Error('Operation registry is closed.');
    if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
      throw new RangeError('timeoutMs must be a non-negative finite number.');
    }
    const id = this.nextId++;
    const deadlineMs = this.clock.now() + timeoutMs;
    this.entries.set(id, { context, deadlineMs, observer });
    this.notePending(deadlineMs);
    return id;
  }

  deadline(id: bigint): number | undefined {
    return this.entries.get(id)?.deadlineMs;
  }

  setFailureDispatcher(id: bigint, dispatcher: (reason: unknown) => void): boolean {
    const entry = this.pendingEntry(id);
    if (entry === undefined) return false;
    entry.failureDispatcher = dispatcher;
    return true;
  }

  clearFailureDispatcher(id: bigint, dispatcher: (reason: unknown) => void): void {
    const entry = this.pendingEntry(id);
    if (entry?.failureDispatcher === dispatcher) entry.failureDispatcher = undefined;
  }

  takeFailure(id: bigint, reason: unknown): boolean {
    const entry = this.take(id);
    if (entry === undefined) return false;
    entry.failureDispatcher = undefined;
    entry.reject?.(reason);
    return true;
  }

  /** The time the next expiry pass has work, as a lower bound (spec 08 §4.2). */
  nextDeadlineMs(): number {
    if (this.entries.size === 0) return Number.POSITIVE_INFINITY;
    if (this.deadlineCursor !== undefined)
      return Math.min(this.lastVisitMs, this.earliestDeadlineMs);
    return Math.min(this.earliestDeadlineMs, this.lastVisitMs + DEADLINE_ROTATION_MS);
  }

  private notePending(deadlineMs: number | undefined): void {
    this.pendingCount++;
    const earlier = deadlineMs !== undefined && deadlineMs < this.earliestDeadlineMs;
    if (earlier) this.earliestDeadlineMs = deadlineMs;
    if (earlier || this.pendingCount === 1) this.onPendingChanged?.();
  }

  complete(id: bigint, value: T, terminalContextRetentionMs = 0): boolean {
    const entry = this.take(id);
    if (entry === undefined) return false;
    entry.failureDispatcher = undefined;
    if (entry.observer !== undefined) entry.observer.operationCompleted(id, value, entry.context);
    else entry.resolve!(value);
    this.retainTerminal(id, entry, terminalContextRetentionMs);
    return true;
  }

  fail(id: bigint, reason: unknown): boolean {
    const entry = this.take(id);
    if (entry === undefined) return false;
    const dispatch = entry.failureDispatcher;
    entry.failureDispatcher = undefined;
    dispatch?.(reason);
    if (entry.observer !== undefined) entry.observer.operationFailed(id, reason, entry.context);
    else entry.reject!(reason);
    return true;
  }

  cancel(id: bigint, message?: string): boolean {
    return this.fail(id, new OperationCancelledError(id, message));
  }

  isPending(id: bigint): boolean {
    return this.pendingEntry(id) !== undefined;
  }

  setContext(id: bigint, context: Context): void {
    const entry = this.pendingEntry(id);
    if (entry !== undefined) entry.context = context;
  }

  context(id: bigint): Context | undefined {
    return this.entries.get(id)?.context;
  }

  clear(reason: unknown): void {
    this.failAll(reason);
    this.entries.clear();
    this.resetEmptyIndex();
  }

  close(reason = 'Operation registry closed.'): void {
    if (this.closed) return;
    this.closed = true;
    for (const id of this.entries.keys()) this.cancel(id, reason);
    this.entries.clear();
    this.resetEmptyIndex();
  }

  failAll(reason: unknown): void {
    for (const id of this.entries.keys()) this.fail(id, reason);
  }

  get size(): number {
    return this.pendingCount;
  }

  expire(nowMs: number, turnDeadlineMs: number): number {
    if (this.entries.size === 0) return 0;
    let expired = 0;
    if (this.deadlineCursor === undefined) {
      this.lastVisitMs = nowMs;
      this.deadlineCursor = this.entries.entries();
      this.scanEarliestDeadlineMs = Number.POSITIVE_INFINITY;
    }
    const cursor = this.deadlineCursor;
    while (this.clock.now() < turnDeadlineMs) {
      const current = cursor.next();
      if (current.done === true) {
        this.deadlineCursor = undefined;
        // Entries registered during the pass are appended and were scanned.
        this.earliestDeadlineMs = this.scanEarliestDeadlineMs;
        break;
      }
      const [id, entry] = current.value;
      if (entry.deadlineMs === undefined) continue;
      if (entry.deadlineMs <= nowMs) {
        if (!this.isPending(id)) {
          this.entries.delete(id);
          if (this.entries.size === 0) this.resetEmptyIndex();
        } else if (this.fail(id, new OperationTimeoutError(id))) expired++;
      } else if (entry.deadlineMs < this.scanEarliestDeadlineMs) {
        this.scanEarliestDeadlineMs = entry.deadlineMs;
      }
    }
    return expired;
  }

  private take(id: bigint): Entry<T, Context> | undefined {
    const entry = this.pendingEntry(id);
    if (entry === undefined) return undefined;
    this.entries.delete(id);
    if (this.entries.size === 0) this.resetEmptyIndex();
    this.pendingCount--;
    if (this.pendingCount === 0) this.onPendingChanged?.();
    return entry;
  }

  private resetEmptyIndex(): void {
    this.deadlineCursor = undefined;
    this.earliestDeadlineMs = Number.POSITIVE_INFINITY;
    this.scanEarliestDeadlineMs = Number.POSITIVE_INFINITY;
  }

  private pendingEntry(id: bigint): Entry<T, Context> | undefined {
    const entry = this.entries.get(id);
    return entry !== undefined && (entry.resolve !== undefined || entry.observer !== undefined)
      ? entry
      : undefined;
  }

  private retainTerminal(id: bigint, entry: Entry<T, Context>, retentionMs: number): void {
    if (this.closed || entry.context === undefined || retentionMs === 0) return;
    entry.resolve = undefined;
    entry.reject = undefined;
    entry.observer = undefined;
    entry.deadlineMs = this.clock.now() + retentionMs;
    this.entries.set(id, entry);
    this.earliestDeadlineMs = Math.min(this.earliestDeadlineMs, entry.deadlineMs);
    this.onPendingChanged?.();
  }
}
