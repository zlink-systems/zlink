export type ZLinkSerialWorkLane = 'application' | 'lifecycle';

export interface ZLinkSerialWorkOptions {
  readonly lane?: ZLinkSerialWorkLane;
  readonly payloadBytes?: number;
  readonly metadataBytes?: number;
}

/** Internal per-owner fairness settings; ingress capacity belongs to the host queue. */
export interface ZLinkSerialSchedulerOptions {
  readonly ownerTimeBudget?: number;
  readonly lifecycleBurstLimit?: number;
}

export const ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS: Required<ZLinkSerialSchedulerOptions> = Object.freeze({
  ownerTimeBudget: 10,
  lifecycleBurstLimit: 8,
});

export interface ZLinkSerialWorkRecord<T> {
  readonly acceptedSequence: bigint;
  readonly lane: ZLinkSerialWorkLane;
  readonly operation: () => Promise<T> | T;
  readonly context?: unknown;
  resolve(value: T): void;
  reject(reason: unknown): void;
  /** Releases this owner's reservation exactly once after the real terminal. */
  release(): void;
  fail(reason: unknown): void;
}

/** Readiness work that must complete without claiming the serial owner. */
export interface ZLinkSerialWorkPreparation {
  prepare(signal: AbortSignal): Promise<void>;
  cancel(): void;
}

type SerialWorkRecord<T> = Omit<ZLinkSerialWorkRecord<T>, 'acceptedSequence'> & {
  acceptedSequence: bigint;
  readonly preparation?: ZLinkSerialWorkPreparation;
  preparationState: 'idle' | 'pending' | 'canceling' | 'ready' | 'failed';
  preparationController?: AbortController;
  preparationError?: unknown;
};

interface ZLinkSerialAdmissionLane {
  readonly records: Array<SerialWorkRecord<unknown>>;
}

/**
 * One event-loop serial execution queue per Spot, Actor mailbox, or Stream session.
 * Application work carries the host permit through the queue until the handler starts.
 */
export class ZLinkSerialExecutionQueue {
  private readonly application: ZLinkSerialAdmissionLane;
  private readonly lifecycle: ZLinkSerialAdmissionLane;
  private readonly ownerTimeBudget: number;
  private readonly lifecycleBurstLimit: number;
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
    validateNonNegative(configured.ownerTimeBudget, 'ownerTimeBudget');
    validatePositive(configured.lifecycleBurstLimit, 'lifecycleBurstLimit');
    this.application = createLane();
    this.lifecycle = createLane();
    this.ownerTimeBudget = configured.ownerTimeBudget;
    this.lifecycleBurstLimit = configured.lifecycleBurstLimit;
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
   * Enqueues a continuation for an already-admitted owner turn.
   */
  submitContinuation<T>(
    operation: () => Promise<T> | T,
    context?: unknown
  ): Promise<T> {
    try {
      return this.admit(operation, { lane: 'application' }, context);
    } catch (error) {
      return Promise.reject(error);
    }
  }

  /**
   * Atomically transfers an already-accepted durable FIFO prefix into this
   * owner queue.
   *
   * @internal
   */
  admitDurablePrefix<T>(
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown,
    preparation?: ZLinkSerialWorkPreparation
  ): Promise<T> {
    return this.admit(
      operation,
      { ...options, lane: 'application' },
      context,
      preparation
    );
  }

  /**
   * Enqueues work that already owns the host-wide application job permit.
   * It remains held through handler terminal completion.
   */
  submitPreAdmitted<T>(
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown
  ): Promise<T> {
    return this.admit(operation, options, context);
  }

  snapshot(): {
    readonly applicationMessages: number;
    readonly applicationBytes: number;
    readonly lifecycleMessages: number;
    readonly lifecycleBytes: number;
  } {
    return {
      applicationMessages: this.application.records.length,
      applicationBytes: 0,
      lifecycleMessages: this.lifecycle.records.length,
      lifecycleBytes: 0
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
    preparation?: ZLinkSerialWorkPreparation
  ): Promise<T> {
    if (this.closed) throw new Error('The serial execution queue is closed.');
    const lane = options.lane ?? 'application';
    const target = lane === 'application' ? this.application : this.lifecycle;
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
      operation,
      context,
      preparation,
      preparationState: preparation === undefined ? 'ready' : 'idle',
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
    let waitingForPreparation = false;
    try {
      for (;;) {
        const selection = this.selectNext();
        if (selection === undefined) break;
        const record = selection.record;
        if (selection.lane === 'lifecycle') {
          const applicationHead = this.application.records[0]!;
          if (this.application.records.length > 0
              && !this.cancelPreparationForRearbitration(applicationHead)) {
            waitingForPreparation = true;
            break;
          }
        }
        if (record.preparationState === 'idle') {
          this.startPreparation(record);
          waitingForPreparation = true;
          break;
        }
        if (record.preparationState === 'pending'
            || record.preparationState === 'canceling') {
          waitingForPreparation = true;
          break;
        }
        this.takeSelected(selection);
        if (record.preparationState === 'failed') {
          record.preparation?.cancel();
          record.fail(record.preparationError);
          continue;
        }
        this.claimStartedAt ??= performance.now();
        try {
          await this.executeRecord(record);
        } catch (error) {
          record.fail(error);
        } finally {
          record.preparation?.cancel();
        }
        if (this.shouldYield()) {
          this.claimStartedAt = undefined;
          await macrotaskBoundary();
        }
      }
    } finally {
      this.draining = false;
      this.claimStartedAt = undefined;
      if (!waitingForPreparation
          && (this.application.records.length > 0 || this.lifecycle.records.length > 0)) {
        this.scheduleDrain();
      } else {
        if (this.application.records.length === 0 && this.lifecycle.records.length === 0) {
          this.resolveIdleWaiters();
        }
      }
    }
  }

  private selectNext(): {
    readonly lane: ZLinkSerialWorkLane;
    readonly record: SerialWorkRecord<unknown>;
  } | undefined {
    const applicationReady = this.application.records.length > 0;
    const lifecycleReady = this.lifecycle.records.length > 0;
    if (!applicationReady && !lifecycleReady) return undefined;
    if (!applicationReady) {
      return { lane: 'lifecycle', record: this.lifecycle.records[0]! };
    }
    if (!lifecycleReady) {
      return { lane: 'application', record: this.application.records[0]! };
    }
    if (!this.lifecycleDebt && this.lifecycleStreak < this.lifecycleBurstLimit) {
      return { lane: 'lifecycle', record: this.lifecycle.records[0]! };
    }
    return { lane: 'application', record: this.application.records[0]! };
  }

  private takeSelected(selection: {
    readonly lane: ZLinkSerialWorkLane;
    readonly record: SerialWorkRecord<unknown>;
  }): ZLinkSerialWorkRecord<unknown> {
    const record = selection.lane === 'lifecycle'
      ? this.takeLifecycle()
      : this.takeApplication();
    if (record !== selection.record) {
      throw new Error('The selected serial work record changed before owner claim.');
    }
    return record;
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

  private startPreparation(record: SerialWorkRecord<unknown>): void {
    const preparation = record.preparation;
    if (preparation === undefined || record.preparationState !== 'idle') return;
    const controller = new AbortController();
    record.preparationController = controller;
    record.preparationError = undefined;
    record.preparationState = 'pending';
    void Promise.resolve()
      .then(() => preparation.prepare(controller.signal))
      .then(
        () => {
          if (record.preparationState === 'canceling' || controller.signal.aborted) {
            preparation.cancel();
            record.preparationState = 'idle';
          } else {
            record.preparationState = 'ready';
          }
          record.preparationController = undefined;
          this.scheduleDrain();
        },
        error => {
          if (record.preparationState === 'canceling' || controller.signal.aborted) {
            preparation.cancel();
            record.preparationState = 'idle';
          } else {
            record.preparationError = error;
            record.preparationState = 'failed';
          }
          record.preparationController = undefined;
          this.scheduleDrain();
        }
      );
  }

  /** Returns false while an outstanding readiness attempt is being canceled. */
  private cancelPreparationForRearbitration(
    record: SerialWorkRecord<unknown>
  ): boolean {
    if (record.preparation === undefined || record.preparationState === 'idle') return true;
    if (record.preparationState === 'ready') {
      record.preparation.cancel();
      record.preparationState = 'idle';
      return true;
    }
    if (record.preparationState === 'failed') return true;
    if (record.preparationState === 'pending') {
      record.preparationState = 'canceling';
      record.preparationController?.abort(
        new Error('Serial work readiness was re-arbitrated by the lifecycle lane.')
      );
    }
    return false;
  }

  private shouldYield(): boolean {
    if (this.ownerTimeBudget === 0 || !this.hasPendingWork) return false;
    return performance.now() - (this.claimStartedAt ?? performance.now()) >= this.ownerTimeBudget;
  }

  private resolveIdleWaiters(): void {
    for (const resolve of this.idleWaiters.splice(0)) resolve();
  }
}

function createLane(): ZLinkSerialAdmissionLane {
  return {
    records: []
  };
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
