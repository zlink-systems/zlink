import {
  captureZLinkSpotSerialTurn,
  isCurrentZLinkSpotSerialTurn,
  runZLinkSpotSerialTurn,
  ZLinkExecutionBarrier,
  type ZLinkExecutionBarrierClaim,
  ZLinkSpotSerialTurn
} from '../execution';
import type { SpotId } from '../../contracts';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import {
  ZLinkBoundedSerialScheduler,
  type ZLinkSerialSchedulerOptions,
  type ZLinkSerialWorkOptions,
  type ZLinkSerialWorkRecord
} from '../execution/serial-scheduler';
import {
  bindApplicationJobPermit,
  hasApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';

export class ZLinkSpotSerialExecutor {
  private readonly scheduler: ZLinkBoundedSerialScheduler;
  private depth = 0;
  private turnSequence = 0;
  private executionBarrier: ZLinkExecutionBarrier | undefined;
  private resumedOwnerTurn: ZLinkSpotSerialTurn | undefined;
  private lastActivityAtMs = Date.now();
  activeTurnId = 0;

  constructor(
    private readonly yieldAllowed = true,
    readonly sourceSpotId?: SpotId,
    schedulerOptions?: ZLinkSerialSchedulerOptions
  ) {
    this.scheduler = new ZLinkBoundedSerialScheduler(
      (record) => this.runQueuedRecord(record),
      {
        ...schedulerOptions,
        capacityError: schedulerOptions?.capacityError ?? (() =>
          createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.WorkerQueueFull,
            'Spot execution queue capacity was exceeded.'
          ))
      }
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

  /** Distinguishes a gate-owning turn from a suspended AsyncLocalStorage tail. */
  isActiveTurn(turn: ZLinkSpotSerialTurn, turnId: number): boolean {
    return this.depth > 0
      && (
        this.resumedOwnerTurn === turn
        || (turnId === this.activeTurnId && !turn.isSuspended)
      );
  }

  setExecutionBarrier(barrier: ZLinkExecutionBarrier): void {
    if (
      this.executionBarrier !== undefined
      && this.executionBarrier !== barrier
    ) {
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
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    if (this.isCurrentTurn) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        'A Spot serial turn cannot implicitly execute another turn for the same owner.'
      );
    }
    return this.enqueueApplicationTurn(operation, workOptions);
  }

  /**
   * Always enqueues `operation` as its own serial turn, even when called
   * from within the currently active turn. Detached completion callbacks use
   * this so they never run inline inside another callback's turn.
   */
  post<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
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
    } = {}
  ): Promise<void> {
    this.lastActivityAtMs = Date.now();
    let barrierClaim: ZLinkExecutionBarrierClaim | undefined;
    try {
      barrierClaim = await this.executionBarrier?.enter();
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
      void submitted.catch(error => {
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

  private async enqueueApplicationTurn<T>(
    operation: () => Promise<T> | T,
    workOptions: ZLinkSerialWorkOptions = {}
  ): Promise<T> {
    this.lastActivityAtMs = Date.now();
    let barrierClaim: ZLinkExecutionBarrierClaim | undefined;
    try {
      barrierClaim = await this.executionBarrier?.enter();
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
    this.lastActivityAtMs = Date.now();
    // Application admission crosses the async barrier boundary before it
    // reaches the scheduler. Give an already-authorized framework turn that
    // same boundary so it cannot overtake admitted application records.
    await Promise.resolve();
    return await this.submitQueuedTurn(operation, workOptions);
  }

  private async enqueueContinuationTurn<T>(
    operation: () => Promise<T> | T
  ): Promise<T> {
    this.lastActivityAtMs = Date.now();
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
      this.lastActivityAtMs = Date.now();
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
      owner.then(() => undefined, () => undefined),
      turn.suspended
    ]);
  }

  private postResume(
    turn: ZLinkSpotSerialTurn,
    resume: () => void,
    reject: (reason: unknown) => void
  ): boolean {
    const barrier = this.executionBarrier;
    const resumeClaim = barrier?.tryEnter();
    if (barrier !== undefined && resumeClaim === undefined) {
      reject(createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        'The yielded Spot turn cannot resume after its execution unit was sealed.'
      ));
      return true;
    }
    turn.bindExecutionClaim(resumeClaim);
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
