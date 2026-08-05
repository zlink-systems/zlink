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

export class ZLinkSpotSerialExecutor {
  private readonly scheduler: ZLinkBoundedSerialScheduler;
  private depth = 0;
  private turnSequence = 0;
  private executionBarrier: ZLinkExecutionBarrier | undefined;
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

  /**
   * Runs `operation` in serial order, one turn at a time. A call made from
   * within the currently active turn of this executor is re-entrant and runs
   * as part of that turn instead of queueing (which would deadlock a turn
   * that awaits the nested result).
   */
  execute<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    if (this.isCurrentTurn) {
      return Promise.resolve().then(operation);
    }
    return this.enqueue(operation, false, workOptions);
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
    return this.enqueue(operation, false, workOptions);
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
    return this.enqueue(operation, true, workOptions);
  }

  private enqueue<T>(
    operation: () => Promise<T> | T,
    resumeExistingClaim: boolean,
    workOptions: ZLinkSerialWorkOptions = {}
  ): Promise<T> {
    return this.scheduleQueuedTurn(operation, resumeExistingClaim, workOptions);
  }

  private async scheduleQueuedTurn<T>(
    operation: () => Promise<T> | T,
    resumeExistingClaim: boolean,
    workOptions: ZLinkSerialWorkOptions
  ): Promise<T> {
    this.lastActivityAtMs = Date.now();
    let barrierClaim: ZLinkExecutionBarrierClaim | undefined;
    try {
      if (!resumeExistingClaim) {
        barrierClaim = await this.executionBarrier?.enter();
      }
      return await this.scheduler.submit(operation, {
        ...workOptions,
        lane: resumeExistingClaim ? 'lifecycle' : workOptions.lane ?? 'application'
      }, barrierClaim);
    } catch (error) {
      barrierClaim?.release();
      throw error;
    }
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
        barrierClaim?.release();
        release?.();
      },
      () => {
        barrierClaim?.release();
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
    void this.enqueue(async () => {
      turn.resetSuspension();
      resume();
      await turn.resumeOwnerUntilNextYield();
    }, true).catch(reject);
    return true;
  }
}
