export type ZLinkSerialWorkLane = 'application' | 'lifecycle';

export interface ZLinkSerialWorkOptions {
  readonly lane?: ZLinkSerialWorkLane;
  readonly payloadBytes?: number;
  readonly metadataBytes?: number;
}

export interface ZLinkSerialSchedulerOptions {
  readonly applicationMessageCapacity?: number;
  readonly applicationByteCapacity?: number;
  readonly lifecycleMessageCapacity?: number;
  readonly lifecycleByteCapacity?: number;
  readonly ownerTimeBudgetMs?: number;
  readonly lifecycleBurstLimit?: number;
  readonly fixedWorkByteCost?: number;
  readonly capacityError?: (lane: ZLinkSerialWorkLane) => unknown;
}

export const ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS: Required<
  Omit<ZLinkSerialSchedulerOptions, 'capacityError'>
> = Object.freeze({
  applicationMessageCapacity: 4_096,
  applicationByteCapacity: 16 * 1024 * 1024,
  lifecycleMessageCapacity: 1_024,
  lifecycleByteCapacity: 4 * 1024 * 1024,
  ownerTimeBudgetMs: 50,
  lifecycleBurstLimit: 8,
  fixedWorkByteCost: 256
});

const MAX_SAFE_INTEGER = Number.MAX_SAFE_INTEGER;

interface SerialLaneState {
  //  Dequeued with a head index instead of shift(): the application lane
  //  admits up to 4096 records, and shift() memmoves the whole backlog on
  //  every turn. Same pattern as the waiter queue below.
  readonly records: Array<SerialWorkRecord<unknown> | undefined>;
  readonly waiters: Array<SerialCapacityWaiter | undefined>;
  readonly messageCapacity: number;
  readonly byteCapacity: number;
  readonly waiterCapacity: number;
  messageCount: number;
  byteCount: number;
  recordHead: number;
  waiterHead: number;
  waiterCount: number;
}

interface SerialCapacityWaiter {
  readonly byteCost: number;
  admit(): void;
}

export interface ZLinkSerialAdmissionWaitOptions {
  readonly timeoutMs: number;
  readonly signal?: AbortSignal;
  readonly timeoutError: () => unknown;
  readonly abortError: () => unknown;
}

export interface ZLinkSerialWorkRecord<T> {
  readonly lane: ZLinkSerialWorkLane;
  readonly byteCost: number;
  readonly operation: () => Promise<T> | T;
  readonly context?: unknown;
  resolve(value: T): void;
  reject(reason: unknown): void;
  release(): void;
  fail(reason: unknown): void;
}

type SerialWorkRecord<T> = ZLinkSerialWorkRecord<T> & {
  readonly _settled: () => boolean;
};

/**
 * One event-loop serial scheduler shared by Spot and Actor execution owners.
 * Reservations include pending and running records until the operation owner
 * explicitly releases the record after handler completion.
 */
export class ZLinkBoundedSerialScheduler {
  private readonly application: SerialLaneState;
  private readonly lifecycle: SerialLaneState;
  private readonly ownerTimeBudgetMs: number;
  private readonly lifecycleBurstLimit: number;
  private readonly fixedWorkByteCost: number;
  private readonly capacityError: (lane: ZLinkSerialWorkLane) => unknown;
  private draining = false;
  private drainScheduled = false;
  private lifecycleStreak = 0;
  private lifecycleDebt = false;
  private claimStartedAt?: number;
  private readonly idleWaiters: Array<() => void> = [];

  constructor(
    private readonly executeRecord: (record: ZLinkSerialWorkRecord<unknown>) => Promise<void>,
    options: ZLinkSerialSchedulerOptions = {}
  ) {
    const configured = {
      ...ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS,
      ...options
    };
    validatePositive(configured.applicationMessageCapacity, 'applicationMessageCapacity');
    validatePositive(configured.applicationByteCapacity, 'applicationByteCapacity');
    validatePositive(configured.lifecycleMessageCapacity, 'lifecycleMessageCapacity');
    validatePositive(configured.lifecycleByteCapacity, 'lifecycleByteCapacity');
    validateNonNegative(configured.ownerTimeBudgetMs, 'ownerTimeBudgetMs');
    validatePositive(configured.lifecycleBurstLimit, 'lifecycleBurstLimit');
    validatePositive(configured.fixedWorkByteCost, 'fixedWorkByteCost');
    this.application = createLane(
      configured.applicationMessageCapacity,
      configured.applicationByteCapacity
    );
    this.lifecycle = createLane(
      configured.lifecycleMessageCapacity,
      configured.lifecycleByteCapacity
    );
    this.ownerTimeBudgetMs = configured.ownerTimeBudgetMs;
    this.lifecycleBurstLimit = configured.lifecycleBurstLimit;
    this.fixedWorkByteCost = configured.fixedWorkByteCost;
    this.capacityError = options.capacityError ?? ((lane) =>
      new Error(`The ${lane} execution queue is full.`));
  }

  submit<T>(
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown
  ): Promise<T> {
    try {
      return this.admit(operation, options, context);
    } catch (error) {
      return Promise.reject(error);
    }
  }

  submitDetached<T>(
    operation: () => Promise<T> | T,
    onError: (error: unknown) => void,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown
  ): void {
    void this.admit(operation, options, context).catch(onError);
  }

  waitAndSubmitDetached<T>(
    operation: () => Promise<T> | T,
    onError: (error: unknown) => void,
    options: ZLinkSerialWorkOptions,
    context: unknown,
    wait: ZLinkSerialAdmissionWaitOptions
  ): Promise<void> {
    if (!Number.isSafeInteger(wait.timeoutMs) || wait.timeoutMs < 1) {
      return Promise.reject(new RangeError('Serial admission timeout must be a positive safe integer.'));
    }
    if (wait.signal?.aborted === true) return Promise.reject(wait.abortError());
    const lane = options.lane ?? 'application';
    const byteCost = this.byteCost(options);
    const target = lane === 'application' ? this.application : this.lifecycle;
    if (canReserve(target, byteCost)) {
      void this.admit(operation, options, context).catch(onError);
      return Promise.resolve();
    }
    if (target.waiterCount >= target.waiterCapacity) {
      return Promise.reject(wait.timeoutError());
    }

    return new Promise<void>((resolve, reject) => {
      let settled = false;
      let index = -1;
      const cleanup = () => {
        clearTimeout(timer);
        wait.signal?.removeEventListener('abort', onAbort);
      };
      const remove = () => {
        if (index < target.waiterHead || target.waiters[index] === undefined) return;
        target.waiters[index] = undefined;
        target.waiterCount -= 1;
        advanceWaiterHead(target);
        compactWaiters(target);
      };
      const fail = (error: unknown) => {
        if (settled) return;
        settled = true;
        remove();
        this.promoteCapacityWaiter(target);
        cleanup();
        reject(error);
      };
      const onAbort = () => fail(wait.abortError());
      const waiter: SerialCapacityWaiter = {
        byteCost,
        admit: () => {
          if (settled) return;
          settled = true;
          cleanup();
          void this.admit(operation, options, context).catch(onError);
          resolve();
        }
      };
      index = target.waiters.length;
      target.waiters.push(waiter);
      target.waiterCount += 1;
      const timer = setTimeout(() => fail(wait.timeoutError()), wait.timeoutMs);
      wait.signal?.addEventListener('abort', onAbort, { once: true });
      if (wait.signal?.aborted === true) onAbort();
    });
  }

  private admit<T>(
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions,
    context?: unknown
  ): Promise<T> {
    const lane = options.lane ?? 'application';
    const byteCost = this.byteCost(options);
    const target = lane === 'application' ? this.application : this.lifecycle;
    if (!reserve(target, byteCost)) {
      throw this.capacityError(lane);
    }

    let settled = false;
    let released = false;
    let resolveResult!: (value: T | PromiseLike<T>) => void;
    let rejectResult!: (reason?: unknown) => void;
    const result = new Promise<T>((resolve, reject) => {
      resolveResult = resolve;
      rejectResult = reject;
    });
    const release = () => {
      if (released) return;
      released = true;
      releaseReservation(target, byteCost);
      this.promoteCapacityWaiter(target);
      this.scheduleDrain();
    };
    const resolve = (value: T) => {
      if (settled) return;
      settled = true;
      resolveResult(value);
    };
    const reject = (reason: unknown) => {
      if (settled) return;
      settled = true;
      rejectResult(reason);
    };
    const record: SerialWorkRecord<T> = {
      lane,
      byteCost,
      operation,
      context,
      resolve,
      reject,
      release,
      fail: (reason) => {
        reject(reason);
        release();
      },
      _settled: () => settled
    };
    target.records.push(record as SerialWorkRecord<unknown>);
    this.scheduleDrain();
    return result;
  }

  snapshot(): {
    readonly applicationMessages: number;
    readonly applicationBytes: number;
    readonly lifecycleMessages: number;
    readonly lifecycleBytes: number;
  } {
    return {
      applicationMessages: this.application.messageCount,
      applicationBytes: this.application.byteCount,
      lifecycleMessages: this.lifecycle.messageCount,
      lifecycleBytes: this.lifecycle.byteCount
    };
  }

  get hasPendingWork(): boolean {
    return this.draining || this.hasReady();
  }

  whenIdle(): Promise<void> {
    if (!this.hasPendingWork) return Promise.resolve();
    return new Promise(resolve => this.idleWaiters.push(resolve));
  }

  private byteCost(options: ZLinkSerialWorkOptions): number {
    const payloadBytes = options.payloadBytes ?? 0;
    const metadataBytes = options.metadataBytes ?? 0;
    validateNonNegative(payloadBytes, 'payloadBytes');
    validateNonNegative(metadataBytes, 'metadataBytes');
    if (payloadBytes > MAX_SAFE_INTEGER - metadataBytes) return MAX_SAFE_INTEGER;
    const payloadAndMetadata = payloadBytes + metadataBytes;
    return payloadAndMetadata > MAX_SAFE_INTEGER - this.fixedWorkByteCost
      ? MAX_SAFE_INTEGER
      : payloadAndMetadata + this.fixedWorkByteCost;
  }

  private scheduleDrain(): void {
    if (this.drainScheduled || this.draining) return;
    this.drainScheduled = true;
    queueMicrotask(() => {
      this.drainScheduled = false;
      void this.drain();
    });
  }

  private async drain(): Promise<void> {
    if (this.draining) return;
    this.draining = true;
    try {
      for (;;) {
        const record = this.takeNext();
        if (record === undefined) break;
        this.claimStartedAt ??= performance.now();
        try {
          await this.executeRecord(record);
        } catch (error) {
          record.fail(error);
        }
        if (this.shouldYield()) {
          this.claimStartedAt = undefined;
          await macrotaskBoundary();
        }
      }
    } finally {
      this.draining = false;
      this.claimStartedAt = undefined;
      if (this.hasReady()) this.scheduleDrain();
      else this.resolveIdleWaiters();
    }
  }

  private takeNext(): ZLinkSerialWorkRecord<unknown> | undefined {
    const applicationReady = pendingRecordCount(this.application) > 0;
    const lifecycleReady = pendingRecordCount(this.lifecycle) > 0;
    if (!applicationReady && !lifecycleReady) return undefined;

    let lane: ZLinkSerialWorkLane;
    if (!applicationReady) {
      lane = 'lifecycle';
    } else if (!lifecycleReady) {
      lane = 'application';
    } else if (!this.lifecycleDebt && this.lifecycleStreak < this.lifecycleBurstLimit) {
      lane = 'lifecycle';
    } else {
      lane = 'application';
    }

    if (lane === 'lifecycle') {
      this.lifecycleStreak += 1;
      if (applicationReady && this.lifecycleStreak >= this.lifecycleBurstLimit) {
        this.lifecycleDebt = true;
      }
      return takeRecord(this.lifecycle);
    }

    this.lifecycleStreak = 0;
    this.lifecycleDebt = false;
    return takeRecord(this.application);
  }

  private shouldYield(): boolean {
    if (this.ownerTimeBudgetMs === 0 || !this.hasReady()) return false;
    return performance.now() - (this.claimStartedAt ?? performance.now()) >= this.ownerTimeBudgetMs;
  }

  private hasReady(): boolean {
    return pendingRecordCount(this.application) > 0
      || pendingRecordCount(this.lifecycle) > 0;
  }

  private resolveIdleWaiters(): void {
    for (const resolve of this.idleWaiters.splice(0)) resolve();
  }

  private promoteCapacityWaiter(lane: SerialLaneState): void {
    advanceWaiterHead(lane);
    const waiter = lane.waiters[lane.waiterHead];
    if (waiter === undefined || !canReserve(lane, waiter.byteCost)) return;
    lane.waiters[lane.waiterHead] = undefined;
    lane.waiterHead += 1;
    lane.waiterCount -= 1;
    advanceWaiterHead(lane);
    compactWaiters(lane);
    waiter.admit();
  }
}

function createLane(messageCapacity: number, byteCapacity: number): SerialLaneState {
  return {
    records: [],
    waiters: [],
    messageCapacity,
    byteCapacity,
    waiterCapacity: messageCapacity,
    messageCount: 0,
    byteCount: 0,
    recordHead: 0,
    waiterHead: 0,
    waiterCount: 0
  };
}

function canReserve(lane: SerialLaneState, byteCost: number): boolean {
  return lane.messageCount < lane.messageCapacity
    && byteCost <= lane.byteCapacity - lane.byteCount;
}

function reserve(lane: SerialLaneState, byteCost: number): boolean {
  if (!canReserve(lane, byteCost)) return false;
  lane.messageCount += 1;
  lane.byteCount += byteCost;
  return true;
}

function pendingRecordCount(lane: SerialLaneState): number {
  return lane.records.length - lane.recordHead;
}

function takeRecord(lane: SerialLaneState): SerialWorkRecord<unknown> | undefined {
  if (lane.recordHead >= lane.records.length) return undefined;
  const record = lane.records[lane.recordHead];
  lane.records[lane.recordHead] = undefined;
  lane.recordHead += 1;
  if (lane.recordHead >= lane.records.length) {
    lane.records.length = 0;
    lane.recordHead = 0;
  } else if (lane.recordHead >= 1024 && lane.recordHead * 2 >= lane.records.length) {
    lane.records.splice(0, lane.recordHead);
    lane.recordHead = 0;
  }
  return record;
}

function advanceWaiterHead(lane: SerialLaneState): void {
  while (
    lane.waiterHead < lane.waiters.length
    && lane.waiters[lane.waiterHead] === undefined
  ) {
    lane.waiterHead += 1;
  }
}

function compactWaiters(lane: SerialLaneState): void {
  if (lane.waiterCount === 0) {
    lane.waiters.length = 0;
    lane.waiterHead = 0;
    return;
  }
  if (lane.waiterHead >= 1024 && lane.waiterHead * 2 >= lane.waiters.length) {
    lane.waiters.splice(0, lane.waiterHead);
    lane.waiterHead = 0;
  }
}

function releaseReservation(lane: SerialLaneState, byteCost: number): void {
  lane.messageCount -= 1;
  lane.byteCount -= byteCost;
}

function validatePositive(value: number, field: string): void {
  if (!Number.isSafeInteger(value) || value < 1) {
    throw new RangeError(`${field} must be a positive safe integer.`);
  }
}

function validateNonNegative(value: number, field: string): void {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(`${field} must be a non-negative safe integer.`);
  }
}

function macrotaskBoundary(): Promise<void> {
  return new Promise(resolve => setImmediate(resolve));
}
