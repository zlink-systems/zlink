export interface OperationClock {
  setTimeout(callback: () => void, delayMs: number): unknown;
  clearTimeout(handle: unknown): void;
}

export class OperationTimeoutError extends Error {
  constructor(readonly operationId: bigint) {
    super(`Operation ${operationId} timed out.`);
    this.name = 'OperationTimeoutError';
  }
}

export class OperationCancelledError extends Error {
  constructor(readonly operationId: bigint, message = 'Operation was cancelled.') {
    super(message);
    this.name = 'OperationCancelledError';
  }
}

export class OperationCapacityExceededError extends Error {
  constructor(readonly maxPendingOperations: number) {
    super(`Operation capacity ${maxPendingOperations} is exhausted.`);
    this.name = 'OperationCapacityExceededError';
  }
}

export interface PendingOperation<T> {
  readonly id: bigint;
  readonly promise: Promise<T>;
}

interface Entry<T> {
  readonly generation: bigint;
  readonly resolve: (value: T | PromiseLike<T>) => void;
  readonly reject: (reason: unknown) => void;
  timer?: unknown;
}

const systemClock: OperationClock = {
  setTimeout(callback, delayMs) {
    const timer = setTimeout(callback, delayMs);
    timer.unref();
    return timer;
  },
  clearTimeout(handle) {
    clearTimeout(handle as NodeJS.Timeout);
  }
};

/** Owns request completion so reply, timeout, cancellation, and shutdown race safely. */
export class OperationRegistry<T> {
  static readonly DEFAULT_MAX_PENDING_OPERATIONS = 65_536;

  private readonly entries = new Map<bigint, Entry<T>>();
  private nextId = 1n;
  private generation = 1n;
  private closed = false;

  constructor(
    private readonly clock: OperationClock = systemClock,
    private readonly maxPendingOperations = OperationRegistry.DEFAULT_MAX_PENDING_OPERATIONS
  ) {
    if (!Number.isSafeInteger(maxPendingOperations) || maxPendingOperations <= 0) {
      throw new RangeError('maxPendingOperations must be a positive safe integer.');
    }
  }

  reserve(timeoutMs: number): PendingOperation<T> {
    if (this.closed) throw new Error('Operation registry is closed.');
    if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
      throw new RangeError('timeoutMs must be a non-negative finite number.');
    }
    if (this.entries.size >= this.maxPendingOperations) {
      throw new OperationCapacityExceededError(this.maxPendingOperations);
    }

    const id = this.nextId++;
    const generation = this.generation;
    let resolve!: Entry<T>['resolve'];
    let reject!: Entry<T>['reject'];
    const promise = new Promise<T>((onResolve, onReject) => {
      resolve = onResolve;
      reject = onReject;
    });
    const entry: Entry<T> = { generation, resolve, reject };
    this.entries.set(id, entry);
    entry.timer = this.clock.setTimeout(
      () => this.rejectIfCurrent(id, generation, new OperationTimeoutError(id)),
      timeoutMs
    );
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

  close(reason = 'Operation registry closed.'): void {
    if (this.closed) return;
    this.closed = true;
    this.generation++;
    const pending = [...this.entries.entries()];
    this.entries.clear();
    for (const [, entry] of pending) this.clearTimer(entry);
    for (const [id, entry] of pending) {
      entry.reject(new OperationCancelledError(id, reason));
    }
  }

  get size(): number {
    return this.entries.size;
  }

  private rejectIfCurrent(id: bigint, generation: bigint, reason: unknown): void {
    const entry = this.entries.get(id);
    if (entry === undefined || entry.generation !== generation) return;
    this.entries.delete(id);
    this.clearTimer(entry);
    entry.reject(reason);
  }

  private take(id: bigint): Entry<T> | undefined {
    const entry = this.entries.get(id);
    if (entry === undefined) return undefined;
    this.entries.delete(id);
    this.clearTimer(entry);
    return entry;
  }

  private clearTimer(entry: Entry<T>): void {
    if (entry.timer === undefined) return;
    this.clock.clearTimeout(entry.timer);
    entry.timer = undefined;
  }
}
