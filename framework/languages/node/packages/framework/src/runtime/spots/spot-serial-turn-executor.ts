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
import { ZLinkActorSerialExecutor } from '../actors/actor-mailbox';

export type ZLinkSpotApplicationTurnContext = ZLinkExecutionBarrierClaim & {
  readonly replay?: (failure?: unknown) => Promise<unknown> | unknown;
};
import {
  bindApplicationJobPermit,
  hasApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';

export class ZLinkSpotSerialTurnExecutor {
  static closingAdmissionError(): ZLinkFrameworkException {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
      'Spot incarnation is closing (committed Close).'
    );
  }
  private readonly scheduler: ZLinkSerialExecutionQueue;
  private depth = 0;
  private executionBarrier: ZLinkExecutionBarrier | undefined;
  // Undefined before the first segment and null when idle after execution starts.
  private activeSegment: { readonly turn: ZLinkSpotSerialTurn } | null | undefined;
  private lastActivityAtMs = performance.now();
  private lifecycleAdmissionClosed = false;

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

  /** Creates a passive Actor mailbox consumed by this Spot's scheduler. */
  createActorExecutor(
    actorId: string,
    sourceSpotId: unknown,
    options?: ZLinkSerialSchedulerOptions
  ): ZLinkActorSerialExecutor {
    return new ZLinkActorSerialExecutor(actorId, sourceSpotId, options, this.scheduler);
  }

  get isCurrentTurn(): boolean {
    return isCurrentZLinkSpotSerialTurn(this);
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

  get currentSegment(): object | undefined {
    return this.isCurrentTurn ? (this.activeSegment ?? undefined) : undefined;
  }

  /** Waits for turns admitted through the execution barrier to complete. */
  whenIdle(): Promise<void> {
    return this.scheduler.whenIdle();
  }

  discardPendingApplication(error: unknown): void {
    this.scheduler.discardPendingApplication(error);
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
      : ZLinkSpotSerialTurnExecutor.closingAdmissionError();
  }

  /**
   * Spot messaging §7: once Close commits Closing, membership work can no longer
   * enter this incarnation while Instance intent messages still queue behind Close.
   */
  setLifecycleAdmissionClosed(closed: boolean): void {
    this.lifecycleAdmissionClosed = closed;
  }

  /** Distinguishes a gate-owning turn from a suspended AsyncLocalStorage tail. */
  isActiveTurn(turn: ZLinkSpotSerialTurn): boolean {
    return this.activeSegment?.turn === turn && !turn.isSuspended;
  }

  setExecutionBarrier(barrier: ZLinkExecutionBarrier): void {
    if (this.executionBarrier !== undefined && this.executionBarrier !== barrier) {
      throw new Error('ZLink Spot serial executor already belongs to another execution barrier.');
    }
    if (this.activeSegment !== undefined && this.executionBarrier === undefined) {
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
    return this.enqueueApplicationTurn(operation, workOptions, undefined, true);
  }

  /**
   * Queues a one-way turn in serial order. Handler completion is reported
   * through `onError` and never blocks the sender turn.
   */
  async postOneWay(
    operation: () => Promise<unknown> | unknown,
    onError: (error: unknown) => void,
    workOptions?: ZLinkSerialWorkOptions,
    admission?: {
      readonly signal?: AbortSignal;
    },
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
      if (admission?.signal?.aborted === true) {
        throw new DOMException('The operation was aborted.', 'AbortError');
      }
      const submitted = hasApplicationJobPermit()
        ? this.scheduler.submitPreAdmitted(operation, workOptions, barrierClaim)
        : this.scheduler.submit(bindApplicationJobPermit(operation), workOptions, barrierClaim);
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
    const scheduler = this.scheduler;
    return this.enqueueFrameworkTurn(operation, {
      ...workOptions,
      get lifecycleOwner() {
        return scheduler.activeLifecycle;
      }
    });
  }

  /** Admits membership work and holds the lifecycle FIFO until its result settles. */
  executeLifecycleOperation<T>(operation: () => Promise<T> | T): Promise<T> {
    const requireAdmission = (): void => {
      if (this.lifecycleAdmissionClosed) {
        throw ZLinkSpotSerialTurnExecutor.closingAdmissionError();
      }
    };
    let entry: ZLinkExecutionBarrierClaim | Promise<ZLinkExecutionBarrierClaim> | undefined;
    try {
      requireAdmission();
      entry = this.executionBarrier?.enter();
    } catch (error) {
      return Promise.reject(error);
    }
    const submit = (claim?: ZLinkExecutionBarrierClaim): Promise<T> => {
      try {
        return this.scheduler.submitLifecycleOperation(() => {
          requireAdmission();
          return operation();
        });
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

  rejectPendingReplays(error: unknown): void {
    const failure =
      error === undefined
        ? createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestFailed,
            'Application execution failed without an error value.'
          )
        : error;
    this.scheduler.visitPendingApplication((record) => {
      const context = record.context as ZLinkSpotApplicationTurnContext | undefined;
      const replay = context?.replay;
      if (replay === undefined) return;
      context!.release();
      Object.assign(record, { context: undefined, operation: () => replay(failure) });
    });
  }

  private async enqueueApplicationTurn<T>(
    operation: () => Promise<T> | T,
    workOptions?: ZLinkSerialWorkOptions,
    replay?: (failure?: unknown) => Promise<T>,
    retainWhilePreparing = false
  ): Promise<T> {
    this.lastActivityAtMs = performance.now();
    let barrierClaim: ZLinkExecutionBarrierClaim | undefined;
    try {
      const entry = this.executionBarrier?.enter();
      if (
        entry instanceof Promise &&
        retainWhilePreparing &&
        this.scheduler.retainsApplicationForLifecycle
      ) {
        try {
          return await this.submitQueuedTurn(operation, workOptions, entry);
        } catch (error) {
          (await entry).release();
          throw error;
        }
      }
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
    workOptions?: ZLinkSerialWorkOptions
  ): Promise<T> {
    this.lastActivityAtMs = performance.now();
    // Application admission crosses the async barrier boundary before it
    // reaches the scheduler. Give an already-authorized framework turn that
    // same boundary so it cannot overtake admitted application records.
    await Promise.resolve();
    return await this.submitQueuedTurn(operation, workOptions);
  }

  private async enqueueContinuationTurn<T>(
    operation: () => Promise<T> | T,
    claim?: ZLinkExecutionBarrierClaim
  ): Promise<T> {
    this.lastActivityAtMs = performance.now();
    await Promise.resolve();
    return await this.scheduler.submitContinuation(operation, claim);
  }

  private submitQueuedTurn<T>(
    operation: () => Promise<T> | T,
    workOptions: ZLinkSerialWorkOptions | undefined,
    barrierClaim?: ZLinkExecutionBarrierClaim | Promise<ZLinkExecutionBarrierClaim>
  ): Promise<T> {
    if (barrierClaim instanceof Promise) {
      const context: Partial<ZLinkExecutionBarrierClaim> = {};
      return this.scheduler.admitDurablePrefix(
        bindApplicationJobPermit(operation),
        workOptions,
        context,
        {
          prepare: async (signal) => {
            const claim = await barrierClaim;
            if (signal.aborted) {
              claim.release();
              throw signal.reason;
            }
            Object.defineProperties(context, Object.getOwnPropertyDescriptors(claim));
          },
          cancel: () => {}
        }
      );
    }
    return hasApplicationJobPermit()
      ? this.scheduler.submitPreAdmitted(operation, workOptions, barrierClaim)
      : this.scheduler.submit(bindApplicationJobPermit(operation), workOptions, barrierClaim);
  }

  private runQueuedRecord(record: ZLinkSerialWorkRecord<unknown>): Promise<void> {
    const context = record.context as ZLinkSpotApplicationTurnContext | undefined;
    if (context?.replay !== undefined && record.operation === context.replay) {
      context.release();
      const executor = this;
      const work = record;
      return Promise.resolve(executor.executionBarrier?.enter()).then((claim) =>
        executor.runTurn(work.operation, work.resolve, work.reject, claim, work.release, work)
      );
    }
    return this.runTurn(
      record.operation,
      record.resolve,
      record.reject,
      record.context as ZLinkExecutionBarrierClaim | undefined,
      record.release,
      record
    );
  }

  yieldPromise<T>(pending: Promise<T>): Promise<T> {
    const turn = this.currentTurn;
    if (turn === undefined) {
      throw new Error('yield requires a framework Spot handler turn.');
    }
    return turn.yieldPromise(pending);
  }

  private async runSegment<T>(turn: ZLinkSpotSerialTurn, operation: () => Promise<T>): Promise<T> {
    const segment = { turn };
    this.activeSegment = segment;
    try {
      return await operation();
    } finally {
      if (this.activeSegment === segment) this.activeSegment = null;
    }
  }

  private runTurn<T>(
    operation: () => Promise<T> | T,
    resolve: (value: T) => void,
    reject: (reason: unknown) => void,
    barrierClaim?: ZLinkExecutionBarrierClaim,
    release?: () => void,
    work?: ZLinkSerialWorkRecord<unknown>
  ): Promise<void> {
    const turn = new ZLinkSpotSerialTurn(
      (resumeTurn, resume, resumeReject) => this.postResume(resumeTurn, resume, resumeReject),
      barrierClaim,
      this.yieldAllowed,
      work
    );
    this.depth += 1;
    this.lastActivityAtMs = performance.now();
    const owner = this.runSegment(turn, () => runZLinkSpotSerialTurn(this, turn, operation));
    turn.bindOwner(owner);
    const releaseOwner = () => {
      this.depth -= 1;
      turn.releaseBoundExecutionClaim();
      release?.();
    };
    owner.then(releaseOwner, releaseOwner);
    return Promise.race([owner.then(resolve, reject), turn.suspended]);
  }

  private postResume(
    turn: ZLinkSpotSerialTurn,
    resume: () => void,
    reject: (reason: unknown) => void
  ): boolean {
    void this.enqueueContinuationTurn(async () => {
      if (!turn.resumeExecutionClaim()) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotMoving,
          'The yielded Spot turn cannot resume after its execution unit was sealed.'
        );
      }
      await this.runSegment(turn, () => {
        turn.resetSuspension();
        resume();
        return turn.resumeOwnerUntilNextYield();
      });
    }, turn.boundExecutionClaim).catch((error) => {
      try {
        turn.executionAllowed();
        reject(error);
      } catch (failure) {
        reject(failure);
      } finally {
        turn.releaseBoundExecutionClaim();
      }
    });
    return true;
  }
}
