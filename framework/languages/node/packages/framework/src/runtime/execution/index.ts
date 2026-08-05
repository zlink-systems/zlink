import { AsyncLocalStorage } from 'node:async_hooks';
import { ZLinkConfigurationException } from '../configuration';
import { isAbortError } from '../abort';

export interface ZLinkRuntimeTaskFailure {
  readonly taskName: string;
  readonly error: unknown;
}

interface ZLinkSpotSerialTurnContext {
  readonly executor: ZLinkSpotSerialExecutorLike;
  readonly turnId: number;
  readonly turn: ZLinkSpotSerialTurn;
}

interface ZLinkSpotSerialExecutorLike {
  readonly activeTurnId: number;
  post<T>(operation: () => Promise<T> | T): Promise<T>;
}

const spotSerialTurnStorage = new AsyncLocalStorage<ZLinkSpotSerialTurnContext>();

export class ZLinkSpotSerialTurn {
  private suspendedResolve: (() => void) | undefined;
  private suspendedPromise: Promise<void> | undefined;
  private suspendSignaled = false;
  private owner: Promise<unknown> | undefined;
  private frameworkOperationsBlocked = false;

  constructor(
    private readonly postResume: (
      turn: ZLinkSpotSerialTurn,
      resume: () => void,
      reject: (reason: unknown) => void
    ) => boolean,
    readonly yieldAllowed: boolean
  ) {}

  get suspended(): Promise<void> {
    if (this.suspendSignaled) {
      return Promise.resolve();
    }
    this.suspendedPromise ??= new Promise((resolve) => {
      this.suspendedResolve = resolve;
    });
    return this.suspendedPromise;
  }

  bindOwner(owner: Promise<unknown>): void {
    this.owner = owner;
  }

  blockFrameworkOperations(): void {
    this.frameworkOperationsBlocked = true;
  }

  ensureFrameworkOperationAllowed(): void {
    if (this.frameworkOperationsBlocked) {
      throw new Error(
        'A Spot handler cannot start another Framework operation after relocationReady().defer().'
      );
    }
  }

  resetSuspension(): void {
    this.suspendSignaled = false;
    this.suspendedResolve = undefined;
    this.suspendedPromise = undefined;
  }

  async yieldPromise<T>(pending: Promise<T>): Promise<T> {
    if (!this.yieldAllowed) {
      throw new ZLinkConfigurationException(
        'yield requires a SpotWide User Spot or Instance Spot application handler.'
      );
    }
    this.signalSuspended();
    const result = await pending;
    await this.awaitResumePermit();
    return result;
  }

  async resumeOwnerUntilNextYield(): Promise<void> {
    const owner = this.owner;
    if (owner === undefined) {
      return;
    }
    await Promise.race([
      owner.then(() => undefined, () => undefined),
      this.suspended
    ]);
  }

  private signalSuspended(): void {
    if (this.suspendSignaled) {
      return;
    }
    this.suspendSignaled = true;
    this.suspendedResolve?.();
  }

  private awaitResumePermit(): Promise<void> {
    return new Promise((resolve, reject) => {
      if (!this.postResume(this, resolve, reject)) {
        reject(new Error('ZLink Spot serial executor is closed.'));
      }
    });
  }
}

export function runZLinkSpotSerialTurn<T>(
  executor: ZLinkSpotSerialExecutorLike,
  turnId: number,
  turn: ZLinkSpotSerialTurn,
  operation: () => Promise<T> | T
): Promise<T> {
  return spotSerialTurnStorage.run(
    { executor, turnId, turn },
    async () => operation()
  );
}

export function captureZLinkSpotSerialTurn(
  executor?: ZLinkSpotSerialExecutorLike
): ZLinkSpotSerialTurn | undefined {
  const current = spotSerialTurnStorage.getStore();
  if (current === undefined) {
    return undefined;
  }
  if (executor !== undefined && current.executor !== executor) {
    return undefined;
  }
  return current.turn;
}

export function requireZLinkYieldTurn(
  capturedTurn?: ZLinkSpotSerialTurn
): ZLinkSpotSerialTurn {
  const turn = capturedTurn ?? captureZLinkSpotSerialTurn();
  if (turn === undefined || !turn.yieldAllowed) {
    throw new ZLinkConfigurationException(
      'yield requires a SpotWide User Spot or Instance Spot application handler.'
    );
  }
  return turn;
}

export interface ZLinkCapturedExecutionTurn {
  yieldPromise<T>(pending: Promise<T>): Promise<T>;
  post(callback: () => void): void;
}

export function captureZLinkExecutionTurn(): ZLinkCapturedExecutionTurn | undefined {
  const current = spotSerialTurnStorage.getStore();
  if (current === undefined) {
    return undefined;
  }
  return {
    yieldPromise: <T>(pending: Promise<T>) => current.turn.yieldPromise(pending),
    post: (callback: () => void) => { void current.executor.post(callback); }
  };
}

export function isCurrentZLinkSpotSerialTurn(executor: ZLinkSpotSerialExecutorLike): boolean {
  const current = spotSerialTurnStorage.getStore();
  return current !== undefined
    && current.executor === executor
    && current.turnId === executor.activeTurnId;
}

export interface ZLinkExecutionBarrierSeal {
  readonly generation: bigint;
}

export interface ZLinkExecutionBarrierClaim {
  release(): void;
}

interface ZLinkExecutionBarrierWaiter {
  readonly resolve: (claim: ZLinkExecutionBarrierClaim) => void;
  readonly reject: (reason: unknown) => void;
}

/**
 * Seals application-lane admission and waits for every active Promise turn to
 * finish. A seal generation prevents a stale abort from reopening a newer
 * lifecycle barrier.
 */
export class ZLinkExecutionBarrier {
  private generation = 0n;
  private activeClaims = 0;
  private currentSeal: ZLinkExecutionBarrierSeal | undefined;
  private readonly admissionWaiters: ZLinkExecutionBarrierWaiter[] = [];
  private readonly quiescenceWaiters = new Set<() => void>();
  private committed = false;

  async enter(): Promise<ZLinkExecutionBarrierClaim> {
    if (this.committed) {
      throw new Error('ZLink execution barrier is committed.');
    }
    if (this.currentSeal !== undefined) {
      return await new Promise<ZLinkExecutionBarrierClaim>((resolve, reject) => {
        this.admissionWaiters.push({ resolve, reject });
      });
    }
    return this.createClaim();
  }

  seal(): ZLinkExecutionBarrierSeal {
    if (this.committed) {
      throw new Error('ZLink execution barrier is committed.');
    }
    if (this.currentSeal !== undefined) {
      throw new Error('ZLink execution barrier is already sealed.');
    }
    const seal = Object.freeze({ generation: ++this.generation });
    this.currentSeal = seal;
    return seal;
  }

  async waitForQuiescence(
    seal: ZLinkExecutionBarrierSeal,
    signal?: AbortSignal
  ): Promise<void> {
    this.requireCurrent(seal);
    if (this.activeClaims === 0) return;
    if (signal?.aborted === true) throw signal.reason;
    await new Promise<void>((resolve, reject) => {
      const complete = () => {
        signal?.removeEventListener('abort', abort);
        resolve();
      };
      const abort = () => {
        this.quiescenceWaiters.delete(complete);
        reject(signal?.reason);
      };
      this.quiescenceWaiters.add(complete);
      signal?.addEventListener('abort', abort, { once: true });
    });
    this.requireCurrent(seal);
  }

  abort(seal: ZLinkExecutionBarrierSeal): boolean {
    if (!this.isCurrent(seal)) return false;
    this.currentSeal = undefined;
    for (const waiter of this.admissionWaiters) waiter.resolve(this.createClaim());
    this.admissionWaiters.length = 0;
    return true;
  }

  commit(seal: ZLinkExecutionBarrierSeal): boolean {
    if (!this.isCurrent(seal)) return false;
    this.currentSeal = undefined;
    this.committed = true;
    const error = new Error('ZLink execution barrier is committed.');
    for (const waiter of this.admissionWaiters) waiter.reject(error);
    this.admissionWaiters.length = 0;
    return true;
  }

  isCurrent(seal: ZLinkExecutionBarrierSeal): boolean {
    return this.currentSeal?.generation === seal.generation;
  }

  private createClaim(): ZLinkExecutionBarrierClaim {
    this.activeClaims++;
    let released = false;
    return {
      release: () => {
        if (released) return;
        released = true;
        this.activeClaims--;
        if (this.activeClaims === 0) {
          for (const waiter of this.quiescenceWaiters) waiter();
          this.quiescenceWaiters.clear();
        }
      }
    };
  }

  private requireCurrent(seal: ZLinkExecutionBarrierSeal): void {
    if (!this.isCurrent(seal)) {
      throw new Error(`ZLink execution barrier generation '${seal.generation}' is stale.`);
    }
  }
}

export type ZLinkRuntimeTaskFailureHandler = (failure: ZLinkRuntimeTaskFailure) => void;
export type ZLinkRuntimeTaskCallback = (signal: AbortSignal) => Promise<void> | void;

export class ZLinkRuntimeTaskErrorSink {
  private readonly handlers = new Set<ZLinkRuntimeTaskFailureHandler>();

  onRuntimeTaskException(handler: ZLinkRuntimeTaskFailureHandler): () => void {
    this.handlers.add(handler);
    return () => {
      this.handlers.delete(handler);
    };
  }

  reportHandlerException(error: unknown): void {
    this.reportRuntimeTaskException('handler', error);
  }

  reportRuntimeTaskException(taskName: string, error: unknown): void {
    for (const handler of this.handlers) {
      try {
        handler({ taskName, error });
      } catch {
        // Error sinks must not create a second unhandled failure path.
      }
    }
  }
}

export class ZLinkRuntimeTaskRunner {
  constructor(
    readonly errorSink: ZLinkRuntimeTaskErrorSink,
    private readonly shutdownSignal: AbortSignal
  ) {}

  runDetached(taskName: string, callback: ZLinkRuntimeTaskCallback): void {
    void this.run(taskName, callback);
  }

  run(taskName: string, callback: ZLinkRuntimeTaskCallback): Promise<void> {
    return Promise.resolve()
      .then(() => callback(this.shutdownSignal))
      .catch((error) => {
        if (isCancellationError(error, this.shutdownSignal)) {
          return;
        }
        this.errorSink.reportRuntimeTaskException(taskName, error);
      });
  }

  reportErrorSinkFailure(_taskName: string, _error: unknown): void {
    // Mirrors the dotnet no-op: a broken error sink must not fail the runtime.
  }
}

export class ZLinkFrameworkExecutionState {
  readonly abortController = new AbortController();
  readonly errorSink = new ZLinkRuntimeTaskErrorSink();
  readonly taskRunner = new ZLinkRuntimeTaskRunner(this.errorSink, this.abortController.signal);
  readonly listenerTasks: Promise<void>[] = [];

  constructor(readonly context: { dispose(): Promise<void> }) {}

  async dispose(): Promise<void> {
    this.abortController.abort();
    await this.waitForListenerTasks();
    await this.context.dispose();
  }

  private async waitForListenerTasks(): Promise<void> {
    if (this.listenerTasks.length === 0) {
      return;
    }
    try {
      await Promise.all(this.listenerTasks);
    } catch (error) {
      if (!isCancellationError(error, this.abortController.signal)) {
        this.errorSink.reportRuntimeTaskException('listener', error);
      }
    }
  }
}

function isCancellationError(error: unknown, signal?: AbortSignal): boolean {
  if (isAbortError(error)) {
    return true;
  }
  return signal?.aborted === true && error === signal.reason;
}
