import { createAbortError } from '../abort';
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
  resolveCaller(value: T): boolean;
  rejectCaller(reason: unknown): boolean;
  isPending(): boolean;
  hasObserver(): boolean;
  setFailureDispatcher(dispatcher: (reason: unknown) => void): void;
  clearFailureDispatcher(dispatcher: (reason: unknown) => void): void;
  setObserver(observer: OperationObserver<T, unknown>, context?: unknown): void;
  deferCompletion(): void;
  reply(value: T, retentionMs?: number): boolean;
  replyFailure(reason: unknown): boolean;
  setSignal(signal: AbortSignal | undefined): void;
  mapCaller(onValue: (value: unknown) => unknown, onFailure: (error: unknown) => unknown): boolean;
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

class RegistryOperation<T, Context> implements OperationHandle<T>, OperationObserver<T, Context> {
  promise?: Promise<T>;
  resolve?: (value: T | PromiseLike<T>) => void;
  reject?: (reason: unknown) => void;
  observer?: OperationObserver<T, Context>;
  failureDispatcher?: (reason: unknown) => void;
  private terminalRetentionMs = 0;
  private signal?: AbortSignal;
  private callerValue?: (value: unknown) => unknown;
  private callerFailure?: (error: unknown) => unknown;
  constructor(
    private readonly registry: OperationRegistry<T, Context>,
    readonly id: bigint,
    public deadlineMs: number | undefined,
    public context: Context | undefined
  ) {}
  complete(value: T, retentionMs = this.terminalRetentionMs): boolean {
    if (this.promise !== undefined) this.observer = undefined;
    return this.registry.complete(this.id, value, retentionMs);
  }
  reply(value: T, retentionMs = 0): boolean {
    if (!this.isPending()) return false;
    if (this.promise === undefined || this.observer === undefined)
      return this.registry.complete(this.id, value, retentionMs);
    const observer = this.observer;
    this.observer = this;
    if (observer !== this) this.terminalRetentionMs = retentionMs;
    observer.operationCompleted(this.id, value, this.context);
    return true;
  }

  replyFailure(reason: unknown): boolean {
    if (!this.isPending()) return false;
    if (this.promise === undefined || this.observer === undefined) return this.fail(reason);
    const observer = this.observer;
    this.observer = this;
    observer.operationFailed(this.id, reason, this.context);
    return true;
  }
  setSignal(signal: AbortSignal | undefined): void {
    this.signal = signal;
    if (signal?.aborted === true) this.handleEvent();
    else signal?.addEventListener('abort', this, { once: true });
  }
  handleEvent(): void {
    this.fail(this.signal?.reason ?? createAbortError());
  }
  private detachSignal(): void {
    this.signal?.removeEventListener('abort', this);
    this.signal = undefined;
  }
  mapCaller(onValue: (value: unknown) => unknown, onFailure: (error: unknown) => unknown): boolean {
    if (this.resolve === undefined || this.reject === undefined) return false;
    const value = this.callerValue;
    const failure = this.callerFailure;
    this.callerValue =
      value === undefined
        ? onValue
        : (reply) => {
            let mapped;
            try {
              mapped = value(reply);
            } catch (error) {
              throw onFailure(error);
            }
            return onValue(mapped);
          };
    this.callerFailure =
      failure === undefined
        ? onFailure
        : (error) => {
            let mapped = error;
            try {
              mapped = failure(error);
            } catch (reason) {
              mapped = reason;
            }
            return onFailure(mapped);
          };
    return true;
  }

  fail(reason: unknown): boolean {
    return this.registry.fail(this.id, reason);
  }
  takeFailure(reason: unknown): boolean {
    return this.registry.takeFailure(this.id, reason);
  }
  isPending(): boolean {
    return this.registry.isPending(this.id);
  }
  setFailureDispatcher(dispatcher: (reason: unknown) => void): void {
    if (!this.registry.setFailureDispatcher(this.id, dispatcher) && this.promise !== undefined)
      void this.promise.then(undefined, dispatcher);
  }
  clearFailureDispatcher(dispatcher: (reason: unknown) => void): void {
    this.registry.clearFailureDispatcher(this.id, dispatcher);
  }
  hasObserver(): boolean {
    return this.observer !== undefined;
  }
  setObserver(observer: OperationObserver<T, Context>, context?: Context): void {
    if (context !== undefined) this.context = context;
    this.observer = observer;
  }
  deferCompletion(): void {
    this.observer = this;
  }
  operationCompleted(): void {}
  operationFailed(): void {}
  resolveCaller(value: T): boolean {
    this.detachSignal();
    const resolve = this.resolve,
      reject = this.reject,
      map = this.callerValue;
    this.resolve = undefined;
    this.reject = undefined;
    this.callerValue = undefined;
    this.callerFailure = undefined;
    if (resolve === undefined) return false;
    try {
      resolve(map === undefined ? value : (map(value) as T));
    } catch (error) {
      reject?.(error);
    }
    return true;
  }
  rejectCaller(reason: unknown): boolean {
    this.detachSignal();
    const reject = this.reject,
      map = this.callerFailure;
    this.resolve = undefined;
    this.reject = undefined;
    this.callerValue = undefined;
    this.callerFailure = undefined;
    if (reject === undefined) return false;
    let failure = reason;
    try {
      if (map !== undefined) failure = map(reason);
    } catch (error) {
      failure = error;
    }
    reject(failure);
    return true;
  }
}
type Entry<T, Context> = RegistryOperation<T, Context>;
interface RetainedContext<Context> {
  context: Context;
  deadlineMs: number;
}
type IndexedOperation<T, Context> = Entry<T, Context> | RetainedContext<Context>;

const DEADLINE_ROTATION_MS = 256;

const systemClock: OperationClock = {
  now: () => performance.now()
};

/** Owns request completion so reply, timeout, cancellation, and shutdown race safely. */
export class OperationRegistry<T, Context = unknown> {
  private readonly entries = new Map<bigint, IndexedOperation<T, Context>>();
  private nextId = 1n;
  private lastVisitMs: number;
  private deadlineCursor?: MapIterator<[bigint, IndexedOperation<T, Context>]>;
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
    private readonly onPendingChanged?: () => void,
    private readonly terminalError: (reason: unknown) => unknown = (reason) => reason
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
    const deadlineMs =
      timeoutOwner === 'registry'
        ? (absoluteDeadlineMs ?? this.clock.now() + timeoutMs)
        : undefined;
    const entry = new RegistryOperation(this, id, deadlineMs, context);
    entry.promise = new Promise<T>((resolve, reject) => {
      entry.resolve = resolve;
      entry.reject = reject;
    });
    this.entries.set(id, entry);
    this.notePending(deadlineMs);
    return entry as PendingOperation<T>;
  }

  handle(id: bigint): OperationHandle<T> {
    const entry = this.entries.get(id);
    if (!(entry instanceof RegistryOperation))
      throw new Error(`Operation ${id} has no active registry entry.`);
    return entry;
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
    const entry = new RegistryOperation(this, id, deadlineMs, context);
    entry.observer = observer;
    this.entries.set(id, entry);
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
    entry.observer = undefined;
    entry.context = undefined;
    entry.rejectCaller(this.terminalError(reason));
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
    const observer = entry.observer;
    entry.observer = undefined;
    if (observer !== undefined) observer.operationCompleted(id, value, entry.context);
    else entry.resolveCaller(value);
    this.retainTerminal(id, entry, terminalContextRetentionMs);
    entry.context = undefined;
    return true;
  }

  reply(id: bigint, value: T, terminalContextRetentionMs = 0): boolean {
    return this.pendingEntry(id)?.reply(value, terminalContextRetentionMs) ?? false;
  }

  fail(id: bigint, reason: unknown): boolean {
    const entry = this.take(id);
    if (entry === undefined) return false;
    reason = this.terminalError(reason);
    const dispatch = entry.failureDispatcher;
    entry.failureDispatcher = undefined;
    dispatch?.(reason);
    const observer = entry.observer;
    entry.observer = undefined;
    observer?.operationFailed(id, reason, entry.context);
    entry.context = undefined;
    entry.rejectCaller(reason);
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
    return entry instanceof RegistryOperation &&
      (entry.resolve !== undefined || entry.observer !== undefined)
      ? entry
      : undefined;
  }

  private retainTerminal(id: bigint, entry: Entry<T, Context>, retentionMs: number): void {
    if (this.closed || entry.context === undefined || retentionMs === 0) return;
    const retained = { context: entry.context, deadlineMs: this.clock.now() + retentionMs };
    this.entries.set(id, retained);
    this.earliestDeadlineMs = Math.min(this.earliestDeadlineMs, retained.deadlineMs);
    this.onPendingChanged?.();
  }
}
