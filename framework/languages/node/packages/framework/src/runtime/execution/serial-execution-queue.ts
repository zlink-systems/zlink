import { macrotaskBoundary } from '../abort';
import {
  diagnosticWorkName,
  guardStateLaneCompletion,
  isStructuralGuardEnabled,
  trackDiagnosticCompletion
} from './state-lane';

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

export const ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS: Required<ZLinkSerialSchedulerOptions> =
  Object.freeze({
    ownerTimeBudget: 10,
    lifecycleBurstLimit: 8
  });

export interface ZLinkSerialWorkRecord<T> {
  readonly acceptedSequence: bigint;
  readonly lane: ZLinkSerialWorkLane;
  operation: () => Promise<T> | T;
  readonly context?: unknown;
  resolve(value: T): void;
  reject(reason: unknown): void;
  /** Releases this owner's reservation exactly once after the real terminal. */
  release(): void;
  fail(reason: unknown): void;
}

/** The active lifecycle owner decides which existing application records can run. */
export interface ZLinkSerialLifecycleContext {
  canExecuteApplication(record: ZLinkSerialWorkRecord<unknown>): boolean;
}

/** Readiness work that must complete without claiming the serial owner. */
export interface ZLinkSerialWorkPreparation {
  prepare(signal: AbortSignal): Promise<void>;
  cancel(): void;
}

type SerialWorkRecord<T> = Omit<ZLinkSerialWorkRecord<T>, 'acceptedSequence'> & {
  acceptedSequence: bigint;
  readonly operationEnvelope: boolean;
  readonly mailbox?: ZLinkSerialExecutionQueue;
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
  private activeLifecycle?: ZLinkSerialWorkRecord<unknown>;
  private activeRecord?: ZLinkSerialWorkRecord<unknown>;
  /** The admitted node itself owns ready membership; no second readiness flag. */
  private ownerMembership?: SerialWorkRecord<unknown>;
  private readonly ownerTimeBudget: number;
  private readonly lifecycleBurstLimit: number;
  private nextAcceptedSequence = 1n;
  private readonly consumer?: { running: boolean; scheduled: boolean };
  private lifecycleStreak = 0;
  private lifecycleDebt = false;
  private claimStartedAt?: number;
  private closed = false;
  private readonly idleWaiters: Array<() => void> = [];

  constructor(
    private readonly executeRecord: (record: ZLinkSerialWorkRecord<unknown>) => Promise<void>,
    options: ZLinkSerialSchedulerOptions = {},
    private readonly sharedOwner?: ZLinkSerialExecutionQueue
  ) {
    const configured = { ...ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS, ...options };
    validateNonNegative(configured.ownerTimeBudget, 'ownerTimeBudget');
    validatePositive(configured.lifecycleBurstLimit, 'lifecycleBurstLimit');
    this.application = createLane();
    this.lifecycle = createLane();
    this.ownerTimeBudget = configured.ownerTimeBudget;
    this.lifecycleBurstLimit = configured.lifecycleBurstLimit;
    if (sharedOwner === undefined) this.consumer = { running: false, scheduled: false };
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
  submitContinuation<T>(operation: () => Promise<T> | T, context?: unknown): Promise<T> {
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
    return this.admit(operation, { ...options, lane: 'application' }, context, preparation);
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

  /** Holds lifecycle FIFO through an operation result while its application turns can run. */
  submitLifecycleOperation<T>(
    operation: () => Promise<T> | T,
    context?: ZLinkSerialLifecycleContext
  ): Promise<T> {
    return this.admit(operation, { lane: 'lifecycle' }, context, undefined, true);
  }

  visitPendingApplication(visitor: (record: ZLinkSerialWorkRecord<unknown>) => void): void {
    for (const record of this.application.records) {
      if (record.mailbox === undefined) visitor(record);
    }
  }

  get hasPendingWork(): boolean {
    return (
      this.consumer?.running === true ||
      this.activeRecord !== undefined ||
      this.activeLifecycle !== undefined ||
      this.application.records.length > 0 ||
      this.lifecycle.records.length > 0
    );
  }

  whenIdle(): Promise<void> {
    if (!this.hasPendingWork) return Promise.resolve();
    return new Promise((resolve) => this.idleWaiters.push(resolve));
  }

  /** Stops new submissions while allowing the accepted FIFO to finish. */
  close(): Promise<void> {
    this.closeAdmission();
    return this.whenIdle();
  }

  /** Stops admission within the lifecycle operation that owns this queue's close. */
  closeAdmission(): void {
    this.closed = true;
  }

  get admissionClosed(): boolean {
    return this.closed;
  }

  private admit<T>(
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions,
    context?: unknown,
    preparation?: ZLinkSerialWorkPreparation,
    operationEnvelope = false
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
      if (this.activeLifecycle === record) {
        this.activeLifecycle = undefined;
        if (this.application.records.length > 0 || this.lifecycle.records.length > 0) {
          this.scheduleDrain();
        } else if (this.consumer?.running !== true) {
          this.resolveIdleWaiters();
        }
      }
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
      operationEnvelope,
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
    if (this.sharedOwner !== undefined) {
      if (this.activeRecord !== undefined) return;
      // Ready membership in the existing owner queue is the only notification state.
      if (this.ownerMembership !== undefined) return;
      this.enqueueMailboxTurn(() => this.consumeMailbox());
      return;
    }
    const consumer = this.consumer!;
    if (consumer.scheduled || consumer.running) return;
    consumer.scheduled = true;
    queueMicrotask(() => {
      consumer.scheduled = false;
      void this.drain();
    });
  }

  /** Notifications carry no payload, admission sequence, result Promise, or host permit. */
  private enqueueMailboxTurn(operation: () => void): void {
    const entry: SerialWorkRecord<unknown> = {
      acceptedSequence: 0n,
      lane: 'application',
      mailbox: this,
      operation,
      operationEnvelope: false,
      preparationState: 'ready',
      resolve: unexpectedMailboxSettlement,
      reject: unexpectedMailboxSettlement,
      release: unexpectedMailboxSettlement,
      fail: unexpectedMailboxSettlement
    };
    const owner = this.sharedOwner!;
    owner.application.records.push(entry);
    this.ownerMembership = entry;
    owner.scheduleDrain();
  }

  /** Called only by the shared scheduler's consumer, never by a producer. */
  private consumeMailbox(): void {
    const selection = this.selectNext();
    if (selection === undefined) {
      if (!this.hasPendingWork) this.resolveIdleWaiters();
      return;
    }
    const record = selection.record;
    if (!this.prepareSelected(selection)) return;
    this.takeSelected(selection);
    if (record.preparationState === 'failed') {
      record.preparation?.cancel();
      record.fail(record.preparationError);
      this.scheduleDrain();
      return;
    }
    this.activeRecord = record;
    // The operation can submit Spot turns, including Yield continuations. Its
    // Actor claim remains held until terminal settlement returns to this gate.
    let terminal: Promise<unknown>;
    try {
      terminal = Promise.resolve(record.operation());
    } catch (error) {
      terminal = Promise.reject(error);
    }
    void terminal.then(
      (value) => this.finishMailbox(record, () => record.resolve(value)),
      (error) => this.finishMailbox(record, () => record.reject(error))
    );
  }

  private finishMailbox(record: SerialWorkRecord<unknown>, settle: () => void): void {
    this.enqueueMailboxTurn(() => {
      settle();
      record.release();
      record.preparation?.cancel();
      this.activeRecord = undefined;
      if (this.selectNext() !== undefined) this.scheduleDrain();
      else if (!this.hasPendingWork) this.resolveIdleWaiters();
    });
  }

  private async drain(): Promise<void> {
    const consumer = this.consumer!;
    if (consumer.running) return;
    consumer.running = true;
    let waitingForPreparation = false;
    try {
      for (;;) {
        const selection = this.selectNext();
        if (selection === undefined) break;
        const record = selection.record;
        if (!this.prepareSelected(selection)) {
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
          if (record.mailbox !== undefined) {
            record.operation();
          } else if (record.operationEnvelope) {
            try {
              const outcome = record.operation();
              void Promise.resolve(outcome).then(
                (value) => {
                  record.resolve(value);
                  record.release();
                },
                (error) => {
                  record.reject(error);
                  record.release();
                }
              );
            } catch (error) {
              record.fail(error);
            }
          } else {
            await this.executeRecord(record);
          }
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
      consumer.running = false;
      this.claimStartedAt = undefined;
      if (!waitingForPreparation && this.selectNext() !== undefined) {
        this.scheduleDrain();
      } else {
        if (!this.hasPendingWork) {
          this.resolveIdleWaiters();
        }
      }
    }
  }

  private prepareSelected(selection: {
    readonly lane: ZLinkSerialWorkLane;
    readonly record: SerialWorkRecord<unknown>;
  }): boolean {
    if (
      selection.lane === 'lifecycle' &&
      this.application.records.length > 0 &&
      !this.cancelPreparationForRearbitration(this.application.records[0]!)
    )
      return false;
    const record = selection.record;
    if (record.preparationState === 'idle') this.startPreparation(record);
    return record.preparationState !== 'pending' && record.preparationState !== 'canceling';
  }

  private selectNext():
    | {
        readonly lane: ZLinkSerialWorkLane;
        readonly record: SerialWorkRecord<unknown>;
      }
    | undefined {
    const lifecycleOwner = this.activeLifecycle?.context as ZLinkSerialLifecycleContext | undefined;
    const applicationRecord =
      lifecycleOwner === undefined
        ? this.application.records[0]
        : this.application.records.find((record) => lifecycleOwner.canExecuteApplication(record));
    const applicationReady = applicationRecord !== undefined;
    const lifecycleReady = this.activeLifecycle === undefined && this.lifecycle.records.length > 0;
    if (!applicationReady && !lifecycleReady) return undefined;
    if (!applicationReady) {
      return { lane: 'lifecycle', record: this.lifecycle.records[0]! };
    }
    if (!lifecycleReady) {
      return { lane: 'application', record: applicationRecord! };
    }
    if (!this.lifecycleDebt && this.lifecycleStreak < this.lifecycleBurstLimit) {
      return { lane: 'lifecycle', record: this.lifecycle.records[0]! };
    }
    return { lane: 'application', record: applicationRecord! };
  }

  private takeSelected(selection: {
    readonly lane: ZLinkSerialWorkLane;
    readonly record: SerialWorkRecord<unknown>;
  }): ZLinkSerialWorkRecord<unknown> {
    const record =
      selection.lane === 'lifecycle'
        ? this.takeLifecycle()
        : this.takeApplication(selection.record);
    if (record !== selection.record) {
      throw new Error('The selected serial work record changed before owner claim.');
    }
    if (selection.record.mailbox !== undefined)
      selection.record.mailbox.ownerMembership = undefined;
    return record;
  }

  private takeLifecycle(): ZLinkSerialWorkRecord<unknown> {
    this.lifecycleStreak += 1;
    if (this.application.records.length > 0 && this.lifecycleStreak >= this.lifecycleBurstLimit) {
      this.lifecycleDebt = true;
    }
    const record = this.lifecycle.records.shift()!;
    this.activeLifecycle = record;
    return record;
  }

  private takeApplication(selected: SerialWorkRecord<unknown>): ZLinkSerialWorkRecord<unknown> {
    this.lifecycleStreak = 0;
    this.lifecycleDebt = false;
    if (this.application.records[0] === selected) return this.application.records.shift()!;
    const index = this.application.records.indexOf(selected);
    return this.application.records.splice(index, 1)[0]!;
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
        (error) => {
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
  private cancelPreparationForRearbitration(record: SerialWorkRecord<unknown>): boolean {
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

if (isStructuralGuardEnabled()) {
  const submit = ZLinkSerialExecutionQueue.prototype.submit;
  ZLinkSerialExecutionQueue.prototype.submit = function <T>(
    this: ZLinkSerialExecutionQueue,
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown
  ): Promise<T> {
    return guardStateLaneCompletion(
      trackDiagnosticCompletion(
        submit.call(this, operation, options, context) as Promise<T>,
        `serial ${options.lane ?? 'application'} ${diagnosticWorkName(operation)}`
      ),
      'serial completion'
    );
  };

  const submitPreAdmitted = ZLinkSerialExecutionQueue.prototype.submitPreAdmitted;
  ZLinkSerialExecutionQueue.prototype.submitPreAdmitted = function <T>(
    this: ZLinkSerialExecutionQueue,
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown
  ): Promise<T> {
    return guardStateLaneCompletion(
      trackDiagnosticCompletion(
        submitPreAdmitted.call(this, operation, options, context) as Promise<T>,
        `serial ${options.lane ?? 'application'} ${diagnosticWorkName(operation)}`
      ),
      'serial completion'
    );
  };

  const submitContinuation = ZLinkSerialExecutionQueue.prototype.submitContinuation;
  ZLinkSerialExecutionQueue.prototype.submitContinuation = function <T>(
    this: ZLinkSerialExecutionQueue,
    operation: () => Promise<T> | T,
    context?: unknown
  ): Promise<T> {
    return guardStateLaneCompletion(
      trackDiagnosticCompletion(
        submitContinuation.call(this, operation, context) as Promise<T>,
        `serial continuation ${diagnosticWorkName(operation)}`
      ),
      'serial completion'
    );
  };

  const admitDurablePrefix = ZLinkSerialExecutionQueue.prototype.admitDurablePrefix;
  ZLinkSerialExecutionQueue.prototype.admitDurablePrefix = function <T>(
    this: ZLinkSerialExecutionQueue,
    operation: () => Promise<T> | T,
    options: ZLinkSerialWorkOptions = {},
    context?: unknown,
    preparation?: ZLinkSerialWorkPreparation
  ): Promise<T> {
    return guardStateLaneCompletion(
      trackDiagnosticCompletion(
        admitDurablePrefix.call(this, operation, options, context, preparation) as Promise<T>,
        `serial durable ${diagnosticWorkName(operation)}`
      ),
      'serial completion'
    );
  };

  const submitLifecycleOperation = ZLinkSerialExecutionQueue.prototype.submitLifecycleOperation;
  ZLinkSerialExecutionQueue.prototype.submitLifecycleOperation = function <T>(
    this: ZLinkSerialExecutionQueue,
    operation: () => Promise<T> | T
  ): Promise<T> {
    return guardStateLaneCompletion(
      trackDiagnosticCompletion(
        submitLifecycleOperation.call(this, operation) as Promise<T>,
        `serial lifecycle ${diagnosticWorkName(operation)}`
      ),
      'serial completion'
    );
  };

  const whenIdle = ZLinkSerialExecutionQueue.prototype.whenIdle;
  ZLinkSerialExecutionQueue.prototype.whenIdle = function (
    this: ZLinkSerialExecutionQueue
  ): Promise<void> {
    return guardStateLaneCompletion(whenIdle.call(this), 'serial completion');
  };
}

function createLane(): ZLinkSerialAdmissionLane {
  return {
    records: []
  };
}

function unexpectedMailboxSettlement(): never {
  throw new Error('A mailbox notification has no independent terminal result.');
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
