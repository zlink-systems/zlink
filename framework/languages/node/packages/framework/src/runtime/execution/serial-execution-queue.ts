export type ZLinkSerialWorkLane = 'application' | 'lifecycle';

export interface ZLinkSerialWorkOptions {
  readonly lane?: ZLinkSerialWorkLane;
  readonly payloadBytes?: number;
  readonly metadataBytes?: number;
}

/** Internal per-owner reservation limits, never a shared ingress budget. */
export interface ZLinkSerialSchedulerOptions {
  readonly applicationMessageCapacity?: number;
  readonly applicationByteCapacity?: number;
  readonly lifecycleMessageCapacity?: number;
  readonly lifecycleByteCapacity?: number;
  readonly ownerTimeBudget?: number;
  readonly lifecycleBurstLimit?: number;
  readonly fixedWorkByteCost?: number;
  readonly capacityError?: (lane: ZLinkSerialWorkLane) => unknown;
}

export const ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS: Required<
  Omit<ZLinkSerialSchedulerOptions, 'capacityError'>
> = Object.freeze({
  applicationMessageCapacity: 1_024,
  applicationByteCapacity: 64 * 1024 * 1024,
  lifecycleMessageCapacity: 128,
  lifecycleByteCapacity: 4 * 1024 * 1024,
  ownerTimeBudget: 10,
  lifecycleBurstLimit: 8,
  fixedWorkByteCost: 256
});

const MAX_SAFE_INTEGER = Number.MAX_SAFE_INTEGER;

export interface ZLinkSerialWorkRecord<T> {
  readonly acceptedSequence: bigint;
  readonly lane: ZLinkSerialWorkLane;
  readonly byteCost: number;
  readonly operation: () => Promise<T> | T;
  readonly context?: unknown;
  resolve(value: T): void;
  reject(reason: unknown): void;
  /** Releases this owner's reservation exactly once after the real terminal. */
  release(): void;
  fail(reason: unknown): void;
}

type SerialWorkRecord<T> = Omit<ZLinkSerialWorkRecord<T>, 'acceptedSequence'> & {
  acceptedSequence: bigint;
};

interface ZLinkSerialAdmissionLane {
  readonly records: Array<SerialWorkRecord<unknown>>;
  readonly messageCapacity: number;
  readonly byteCapacity: number;
  messageCount: number;
  byteCount: number;
}

/**
 * One event-loop serial execution queue per Spot, Actor mailbox, or Stream session.
 * Its reservations are owner-local and span queued plus running work until the
 * execution owner reaches its terminal transition.
 */
export class ZLinkSerialExecutionQueue {
  private readonly application: ZLinkSerialAdmissionLane;
  private readonly lifecycle: ZLinkSerialAdmissionLane;
  private readonly ownerTimeBudget: number;
  private readonly lifecycleBurstLimit: number;
  private readonly fixedWorkByteCost: number;
  private readonly capacityError: (lane: ZLinkSerialWorkLane) => unknown;
  private nextAcceptedSequence = 1n;
  private draining = false;
  private drainScheduled = false;
  private lifecycleStreak = 0;
  private lifecycleDebt = false;
  private claimStartedAt?: number;
  private closed = false;
  private readonly idleWaiters: Array<() => void> = [];

  constructor(
    private readonly executeRecord: (record: ZLinkSerialWorkRecord<unknown>) => Promise<void>,
    options: ZLinkSerialSchedulerOptions = {}
  ) {
    const configured = { ...ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS, ...options };
    validatePositive(configured.applicationMessageCapacity, 'applicationMessageCapacity');
    validatePositive(configured.applicationByteCapacity, 'applicationByteCapacity');
    validatePositive(configured.lifecycleMessageCapacity, 'lifecycleMessageCapacity');
    validatePositive(configured.lifecycleByteCapacity, 'lifecycleByteCapacity');
    validateNonNegative(configured.ownerTimeBudget, 'ownerTimeBudget');
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
    this.ownerTimeBudget = configured.ownerTimeBudget;
    this.lifecycleBurstLimit = configured.lifecycleBurstLimit;
    this.fixedWorkByteCost = configured.fixedWorkByteCost;
    this.capacityError = options.capacityError
      ?? (lane => new Error(`The ${lane} execution queue is full.`));
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
    void this.submit(operation, options, context).catch(onError);
  }

  /**
   * Enqueues a continuation for an already-reserved owner turn. It preserves
   * application FIFO order without taking a second owner-local reservation.
   */
  submitContinuation<T>(
    operation: () => Promise<T> | T,
    context?: unknown
  ): Promise<T> {
    try {
      return this.admit(operation, { lane: 'application' }, context, false);
    } catch (error) {
      return Promise.reject(error);
    }
  }

  /**
   * Enqueues work that already owns the host-wide application job permit.
   * The shared permit is the capacity authority, so an owner-local reservation
   * must not reject or double-charge the same job before its handler starts.
   */
  submitPreAdmitted<T>(
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown
  ): Promise<T> {
    return this.admit(operation, options, context, false);
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
    return this.draining
      || this.application.records.length > 0
      || this.lifecycle.records.length > 0;
  }

  whenIdle(): Promise<void> {
    if (!this.hasPendingWork) return Promise.resolve();
    return new Promise(resolve => this.idleWaiters.push(resolve));
  }

  /** Stops new submissions while allowing the accepted FIFO to finish. */
  close(): Promise<void> {
    this.closed = true;
    return this.whenIdle();
  }

  private admit<T>(
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions,
    context?: unknown,
    reserveCapacity = true
  ): Promise<T> {
    if (this.closed) throw new Error('The serial execution queue is closed.');
    const lane = options.lane ?? 'application';
    const byteCost = reserveCapacity ? this.byteCost(options) : 0;
    const target = lane === 'application' ? this.application : this.lifecycle;
    if (reserveCapacity && !reserve(target, byteCost)) throw this.capacityError(lane);

    let settled = false;
    let released = false;
    let resolveResult!: (value: T | PromiseLike<T>) => void;
    let rejectResult!: (reason?: unknown) => void;
    const result = new Promise<T>((resolve, reject) => {
      resolveResult = resolve;
      rejectResult = reject;
    });
    const release = (): void => {
      if (released) return;
      released = true;
      if (reserveCapacity) releaseReservation(target, byteCost);
    };
    const resolve = (value: T): void => {
      if (settled) return;
      settled = true;
      resolveResult(value);
    };
    const reject = (reason: unknown): void => {
      if (settled) return;
      settled = true;
      rejectResult(reason);
    };
    const record: SerialWorkRecord<T> = {
      acceptedSequence: 0n,
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
      }
    };
    const previousLength = target.records.length;
    try {
      record.acceptedSequence = this.nextAcceptedSequence;
      target.records.push(record as SerialWorkRecord<unknown>);
      this.nextAcceptedSequence += 1n;
    } catch (error) {
      target.records.length = previousLength;
      record.acceptedSequence = 0n;
      release();
      throw error;
    }
    this.scheduleDrain();
    return result;
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
      if (this.application.records.length > 0 || this.lifecycle.records.length > 0) {
        this.scheduleDrain();
      } else {
        this.resolveIdleWaiters();
      }
    }
  }

  private takeNext(): ZLinkSerialWorkRecord<unknown> | undefined {
    const applicationReady = this.application.records.length > 0;
    const lifecycleReady = this.lifecycle.records.length > 0;
    if (!applicationReady && !lifecycleReady) return undefined;
    if (!applicationReady) return this.takeLifecycle();
    if (!lifecycleReady) return this.takeApplication();
    if (!this.lifecycleDebt && this.lifecycleStreak < this.lifecycleBurstLimit) {
      return this.takeLifecycle();
    }
    return this.takeApplication();
  }

  private takeLifecycle(): ZLinkSerialWorkRecord<unknown> {
    this.lifecycleStreak += 1;
    if (
      this.application.records.length > 0
      && this.lifecycleStreak >= this.lifecycleBurstLimit
    ) {
      this.lifecycleDebt = true;
    }
    return this.lifecycle.records.shift()!;
  }

  private takeApplication(): ZLinkSerialWorkRecord<unknown> {
    this.lifecycleStreak = 0;
    this.lifecycleDebt = false;
    return this.application.records.shift()!;
  }

  private shouldYield(): boolean {
    if (this.ownerTimeBudget === 0 || !this.hasPendingWork) return false;
    return performance.now() - (this.claimStartedAt ?? performance.now()) >= this.ownerTimeBudget;
  }

  private resolveIdleWaiters(): void {
    for (const resolve of this.idleWaiters.splice(0)) resolve();
  }
}

function createLane(messageCapacity: number, byteCapacity: number): ZLinkSerialAdmissionLane {
  return {
    records: [],
    messageCapacity,
    byteCapacity,
    messageCount: 0,
    byteCount: 0
  };
}

function reserve(lane: ZLinkSerialAdmissionLane, byteCost: number): boolean {
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

function releaseReservation(lane: ZLinkSerialAdmissionLane, byteCost: number): void {
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
