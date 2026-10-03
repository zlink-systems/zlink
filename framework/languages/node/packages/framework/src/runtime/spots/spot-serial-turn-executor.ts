import {
  captureZLinkSpotSerialTurn,
  isCurrentZLinkSpotSerialTurn,
  runZLinkSpotSerialTurn,
  ZLinkExecutionBarrier,
  type ZLinkExecutionBarrierClaim,
  ZLinkSpotSerialTurn
} from '../execution';
import type { SpotId } from '../../contracts';
import type { ZLinkFrameworkException } from '../../contracts/Errors/ZLinkFrameworkException';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import {
  ZLinkSerialExecutionQueue,
  type ZLinkSerialSchedulerOptions,
  type ZLinkSerialWorkOptions,
  type ZLinkSerialWorkRecord
} from '../execution/serial-execution-queue';
import type { ZLinkSerialLifecycleContext } from '../execution/serial-execution-queue';

export type ZLinkSpotApplicationTurnContext = ZLinkExecutionBarrierClaim & {
  readonly replay?: (failure?: unknown) => Promise<unknown> | unknown;
};
import {
  bindApplicationJobPermit,
  hasApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';

export class ZLinkSpotSerialTurnExecutor {
  private readonly scheduler: ZLinkSerialExecutionQueue;
  private depth = 0;
  private turnSequence = 0;
  private executionBarrier: ZLinkExecutionBarrier | undefined;
  private resumedOwnerTurn: ZLinkSpotSerialTurn | undefined;
  private lastActivityAtMs = performance.now();
  private lifecycleAdmissionClosed = false;
  activeTurnId = 0;

  constructor(
    private readonly yieldAllowed = true,
    readonly sourceSpotId?: SpotId,
    schedulerOptions?: ZLinkSerialSchedulerOptions
  ) {
    this.scheduler = new ZLinkSerialExecutionQueue(
      (record) => this.runQueuedRecord(record),
      schedulerOptions
    );
  }

  get isExecuting(): boolean {
    return this.depth > 0;
  }

  get isCurrentTurn(): boolean {
    return this.depth > 0 && isCurrentZLinkSpotSerialTurn(this);
  }

  get hasPendingWork(): boolean {
    return this.scheduler.hasPendingWork;
  }

  get lastActivityAt(): number {
    return this.lastActivityAtMs;
  }

  get currentTurn(): ZLinkSpotSerialTurn | undefined {
    return captureZLinkSpotSerialTurn(this);
  }

  /** Waits for turns admitted through the execution barrier to complete. */
  whenIdle(): Promise<void> {
    return this.scheduler.whenIdle();
  }

  closeAdmission(): void {
    this.scheduler.closeAdmission();
  }

  /**
   * Spot messaging §7 step 3: after Close ends this queue's admission, an Instance
   * intent message is refused before admission as a released owner fence, and any
   * other message finds no incarnation. Undefined while admission is open.
   */
  applicationAdmissionRefusal(instanceIntent: boolean): ZLinkFrameworkException | undefined {
    if (!this.scheduler.admissionClosed) return undefined;
    return instanceIntent
      ? createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotMoving,
          'The Spot incarnation released its authority before admitting this Instance intent message.'
        )
      : createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
          'The Spot incarnation is closed.'
        );
  }

  /**
   * Spot messaging §7: once Close commits Closing, membership work can no longer
   * enter this incarnation while Instance intent messages still queue behind Close.
   */
  setLifecycleAdmissionClosed(closed: boolean): void {
    this.lifecycleAdmissionClosed = closed;
  }

  /** Distinguishes a gate-owning turn from a suspended AsyncLocalStorage tail. */
  isActiveTurn(turn: ZLinkSpotSerialTurn, turnId: number): boolean {
    return (
      this.depth > 0 &&
      (this.resumedOwnerTurn === turn || (turnId === this.activeTurnId && !turn.isSuspended))
    );
  }

  setExecutionBarrier(barrier: ZLinkExecutionBarrier): void {
    if (this.executionBarrier !== undefined && this.executionBarrier !== barrier) {
      throw new Error('ZLink Spot serial executor already belongs to another execution barrier.');
    }
    if (this.turnSequence !== 0 && this.executionBarrier === undefined) {
      throw new Error('ZLink execution barrier must be attached before the first serial turn.');
    }
    this.executionBarrier = barrier;
  }

  /** Runs `operation` in serial order, one turn at a time. */
  execute<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions,
    replay?: (failure?: unknown) => Promise<T>
  ): Promise<T> {
    if (this.isCurrentTurn) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        'A Spot serial turn cannot implicitly execute another turn for the same owner.'
      );
    }
    return this.enqueueApplicationTurn(operation, workOptions, replay);
  }

  /**
   * Always enqueues `operation` as its own serial turn, even when called
   * from within the currently active turn. Detached completion callbacks use
   * this so they never run inline inside another callback's turn.
   */
  post<T>(operation: () => Promise<T> | T, workOptions?: ZLinkSerialWorkOptions): Promise<T> {
    return this.enqueueApplicationTurn(operation, workOptions);
  }

  /**
   * Queues a one-way turn in serial order. Handler completion is reported
   * through `onError` and never blocks the sender turn.
   */
  async postOneWay(
    operation: () => Promise<unknown> | unknown,
    onError: (error: unknown) => void,
    workOptions: ZLinkSerialWorkOptions = {},
    admission: {
      readonly signal?: AbortSignal;
    } = {},
    replay?: (failure?: unknown) => Promise<unknown>
  ): Promise<void> {
    this.lastActivityAtMs = performance.now();
    let barrierClaim: ZLinkExecutionBarrierClaim | undefined;
    try {
      const entry = this.executionBarrier?.enter();
      barrierClaim = entry instanceof Promise ? await entry : entry;
      const refusal = this.applicationAdmissionRefusal(replay !== undefined);
      if (refusal !== undefined) throw refusal;
      if (barrierClaim !== undefined && replay !== undefined) {
        Object.assign(barrierClaim, { replay: bindApplicationJobPermit(replay) });
      }
      if (admission.signal?.aborted === true) {
        throw new DOMException('The operation was aborted.', 'AbortError');
      }
      const options = {
        ...workOptions,
        lane: workOptions.lane ?? 'application'
      };
      const boundOperation = bindApplicationJobPermit(operation);
      const submitted = hasApplicationJobPermit()
        ? this.scheduler.submitPreAdmitted(boundOperation, options, barrierClaim)
        : this.scheduler.submit(boundOperation, options, barrierClaim);
      void submitted.catch((error) => {
        barrierClaim?.release();
        onError(error);
      });
      // Let an empty owner queue begin its terminal handler turn before the
      // ingress record is allowed to advance to the next owner.
      await Promise.resolve();
    } catch (error) {
      barrierClaim?.release();
      throw error;
    }
  }

  /**
   * Enqueues the framework-owned turn that completes a boundary while normal
   * application admission remains sealed. Callers must release the matching
   * barrier seal after this turn finishes.
   */
  postBarrierTurn<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    return this.enqueueFrameworkTurn(operation, workOptions);
  }

  /** Admits membership work and holds the lifecycle FIFO until its result settles. */
  executeLifecycleOperation<T>(operation: () => Promise<T> | T): Promise<T> {
    let entry: ZLinkExecutionBarrierClaim | Promise<ZLinkExecutionBarrierClaim> | undefined;
    try {
      if (this.lifecycleAdmissionClosed) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestRejected,
          'Spot lifecycle admission is closed by a committed Close.'
        );
      }
      entry = this.executionBarrier?.enter();
    } catch (error) {
      return Promise.reject(error);
    }
    const submit = (claim?: ZLinkExecutionBarrierClaim): Promise<T> => {
      try {
        return this.scheduler.submitLifecycleOperation(operation);
      } catch (error) {
        return Promise.reject(error);
      } finally {
        claim?.release();
      }
    };
    return entry instanceof Promise ? entry.then(submit) : submit(entry);
  }

  /** Runs owner control work that establishes or completes its own admission seal. */
  executeControlLifecycleOperation<T>(
    operation: () => Promise<T> | T,
    context?: ZLinkSerialLifecycleContext
  ): Promise<T> {
    try {
      return this.scheduler.submitLifecycleOperation(operation, context);
    } catch (error) {
      return Promise.reject(error);
    }
  }

  visitPendingApplication(visitor: (record: ZLinkSerialWorkRecord<unknown>) => void): void {
    this.scheduler.visitPendingApplication(visitor);
  }

  private async enqueueApplicationTurn<T>(
    operation: () => Promise<T> | T,
    workOptions: ZLinkSerialWorkOptions = {},
    replay?: (failure?: unknown) => Promise<T>
  ): Promise<T> {
    this.lastActivityAtMs = performance.now();
    let barrierClaim: ZLinkExecutionBarrierClaim | undefined;
    try {
      const entry = this.executionBarrier?.enter();
      barrierClaim = entry instanceof Promise ? await entry : entry;
      const refusal = this.applicationAdmissionRefusal(replay !== undefined);
      if (refusal !== undefined) throw refusal;
      if (barrierClaim !== undefined && replay !== undefined) {
        Object.assign(barrierClaim, { replay: bindApplicationJobPermit(replay) });
      }
      return await this.submitQueuedTurn(operation, workOptions, barrierClaim);
    } catch (error) {
      barrierClaim?.release();
      throw error;
    }
  }

  private async enqueueFrameworkTurn<T>(
    operation: () => Promise<T> | T,
    workOptions: ZLinkSerialWorkOptions = {}
  ): Promise<T> {
    this.lastActivityAtMs = performance.now();
    // Application admission crosses the async barrier boundary before it
    // reaches the scheduler. Give an already-authorized framework turn that
    // same boundary so it cannot overtake admitted application records.
    await Promise.resolve();
    return await this.submitQueuedTurn(operation, workOptions);
  }

  private async enqueueContinuationTurn<T>(operation: () => Promise<T> | T): Promise<T> {
    this.lastActivityAtMs = performance.now();
    await Promise.resolve();
    return await this.scheduler.submitContinuation(operation);
  }

  private submitQueuedTurn<T>(
    operation: () => Promise<T> | T,
    workOptions: ZLinkSerialWorkOptions,
    barrierClaim?: ZLinkExecutionBarrierClaim
  ): Promise<T> {
    const options = {
      ...workOptions,
      lane: workOptions.lane ?? 'application'
    };
    const boundOperation = bindApplicationJobPermit(operation);
    return hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(boundOperation, options, barrierClaim)
      : this.scheduler.submit(boundOperation, options, barrierClaim);
  }

  private runQueuedRecord(record: ZLinkSerialWorkRecord<unknown>): Promise<void> {
    const context = record.context as ZLinkSpotApplicationTurnContext | undefined;
    if (context?.replay !== undefined && record.operation === context.replay) {
      context.release();
      return Promise.resolve(this.executionBarrier?.enter()).then((claim) =>
        this.runTurn(
          record.operation,
          (value) => record.resolve(value),
          (error) => record.reject(error),
          claim,
          record.release
        )
      );
    }
    return this.runTurn(
      record.operation,
      (value) => record.resolve(value),
      (error) => record.reject(error),
      record.context as ZLinkExecutionBarrierClaim | undefined,
      record.release
    );
  }

  yieldPromise<T>(pending: Promise<T>): Promise<T> {
    const turn = this.currentTurn;
    if (turn === undefined) {
      throw new Error('yield requires a framework Spot handler turn.');
    }
    return turn.yieldPromise(pending);
  }

  private runTurn<T>(
    operation: () => Promise<T> | T,
    resolve: (value: T) => void,
    reject: (reason: unknown) => void,
    barrierClaim?: ZLinkExecutionBarrierClaim,
    release?: () => void
  ): Promise<void> {
    const turn = new ZLinkSpotSerialTurn(
      (resumeTurn, resume, resumeReject) => this.postResume(resumeTurn, resume, resumeReject),
      barrierClaim,
      this.yieldAllowed
    );
    const wrapped = async () => {
      this.depth += 1;
      this.lastActivityAtMs = performance.now();
      this.turnSequence += 1;
      const turnId = this.turnSequence;
      this.activeTurnId = turnId;
      try {
        return await runZLinkSpotSerialTurn(this, turnId, turn, operation);
      } finally {
        this.depth -= 1;
        this.activeTurnId = 0;
      }
    };
    const owner = wrapped();
    turn.bindOwner(owner);
    owner.then(
      () => {
        turn.releaseBoundExecutionClaim();
        release?.();
      },
      () => {
        turn.releaseBoundExecutionClaim();
        release?.();
      }
    );
    owner.then(resolve, reject);
    return Promise.race([
      owner.then(
        () => undefined,
        () => undefined
      ),
      turn.suspended
    ]);
  }

  private postResume(
    turn: ZLinkSpotSerialTurn,
    resume: () => void,
    reject: (reason: unknown) => void
  ): boolean {
    if (!turn.resumeExecutionClaim()) {
      reject(
        createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotMoving,
          'The yielded Spot turn cannot resume after its execution unit was sealed.'
        )
      );
      return true;
    }
    void this.enqueueContinuationTurn(async () => {
      this.resumedOwnerTurn = turn;
      try {
        turn.resetSuspension();
        resume();
        await turn.resumeOwnerUntilNextYield();
      } finally {
        if (this.resumedOwnerTurn === turn) this.resumedOwnerTurn = undefined;
      }
    }).catch((error) => {
      turn.releaseBoundExecutionClaim();
      reject(error);
    });
    return true;
  }
}
