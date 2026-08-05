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
  readonly records: SerialWorkRecord<unknown>[];
  readonly messageCapacity: number;
  readonly byteCapacity: number;
  messageCount: number;
  byteCount: number;
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
    const lane = options.lane ?? 'application';
    const byteCost = this.byteCost(options);
    const target = lane === 'application' ? this.application : this.lifecycle;
    if (!reserve(target, byteCost)) {
      return Promise.reject(this.capacityError(lane));
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
    const applicationReady = this.application.records.length > 0;
    const lifecycleReady = this.lifecycle.records.length > 0;
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
      return this.lifecycle.records.shift();
    }

    this.lifecycleStreak = 0;
    this.lifecycleDebt = false;
    return this.application.records.shift();
  }

  private shouldYield(): boolean {
    if (this.ownerTimeBudgetMs === 0 || !this.hasReady()) return false;
    return performance.now() - (this.claimStartedAt ?? performance.now()) >= this.ownerTimeBudgetMs;
  }

  private hasReady(): boolean {
    return this.application.records.length > 0 || this.lifecycle.records.length > 0;
  }

  private resolveIdleWaiters(): void {
    for (const resolve of this.idleWaiters.splice(0)) resolve();
  }
}

function createLane(messageCapacity: number, byteCapacity: number): SerialLaneState {
  return {
    records: [],
    messageCapacity,
    byteCapacity,
    messageCount: 0,
    byteCount: 0
  };
}

function reserve(lane: SerialLaneState, byteCost: number): boolean {
  if (
    lane.messageCount >= lane.messageCapacity
    || byteCost > lane.byteCapacity - lane.byteCount
  ) {
    return false;
  }
  lane.messageCount += 1;
  lane.byteCount += byteCost;
  return true;
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
