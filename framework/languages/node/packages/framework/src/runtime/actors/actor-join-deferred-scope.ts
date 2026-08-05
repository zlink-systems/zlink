import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { AsyncLocalStorage } from 'node:async_hooks';
import { ZLinkConfigurationException } from '../configuration';

const MAX_DEFERRED_JOINS = 64;
const MAX_DEFERRED_JOIN_REQUEST_BYTES = 1024 * 1024;
const MAX_DEFERRED_JOIN_BYTES = 8 * 1024 * 1024;

interface DeferredActorJoin {
  readonly requestBytes: number;
  /** Starts target admission after the handler terminal, before reply admission. */
  readonly prepare?: () => Promise<void> | void;
  readonly execute: () => Promise<void>;
  readonly discard: () => Promise<void> | void;
}

class ActorJoinRegistrationScope {
  private readonly intents: DeferredActorJoin[] = [];
  private totalBytes = 0;
  private open = true;
  private prepared = false;

  // Ledger 2.3: a deferred join runs in registration order right after the
  // handler terminal and before the next queued turn, matching the C++ serial
  // queue's deferred-after-active list.

  register(intent: DeferredActorJoin): void {
    if (!this.open) {
      discardWithoutWaiting(intent);
      throw new ZLinkConfigurationException(
        'Actor join defer requires an open Framework actor handler scope.'
      );
    }
    if (this.intents.length >= MAX_DEFERRED_JOINS) {
      discardWithoutWaiting(intent);
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
        `An actor handler may defer at most ${MAX_DEFERRED_JOINS} joins.`
      );
    }
    if (intent.requestBytes > MAX_DEFERRED_JOIN_REQUEST_BYTES) {
      discardWithoutWaiting(intent);
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
        `A deferred actor join request may use at most ${MAX_DEFERRED_JOIN_REQUEST_BYTES} bytes.`
      );
    }
    if (this.totalBytes + intent.requestBytes > MAX_DEFERRED_JOIN_BYTES) {
      discardWithoutWaiting(intent);
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
        `Deferred actor join requests may use at most ${MAX_DEFERRED_JOIN_BYTES} bytes per handler.`
      );
    }
    this.intents.push(intent);
    this.totalBytes += intent.requestBytes;
  }

  sealAfterHandlerTerminal(): void {
    this.open = false;
  }

  async prepareSealedJoins(): Promise<void> {
    if (this.prepared) {
      return;
    }
    this.prepared = true;
    for (const intent of this.intents) {
      await intent.prepare?.();
    }
  }

  async activateSealedJoins(): Promise<void> {
    if (this.intents.length === 0) {
      return;
    }
    await this.prepareSealedJoins();
    // A deferred callback failure cannot replace the handler result with a
    // second request outcome. The dispatcher releases that result only after
    // this terminal work, while the next queued turn remains behind it.
    await this.execute().catch(() => undefined);
  }

  async discard(): Promise<void> {
    this.open = false;
    await Promise.allSettled(this.intents.map((intent) => intent.discard()));
  }

  private async execute(): Promise<void> {
    for (const intent of this.intents) {
      await intent.execute();
    }
  }
}

const actorJoinScope = new AsyncLocalStorage<ActorJoinRegistrationScope>();

export function deferActorJoin(intent: DeferredActorJoin): void {
  const scope = actorJoinScope.getStore();
  if (scope === undefined) {
    discardWithoutWaiting(intent);
    throw new ZLinkConfigurationException(
      'Actor join defer requires an open Framework actor handler scope.'
    );
  }
  scope.register(intent);
}

export function runActorHandlerWithDeferredJoins<T>(
  handler: () => Promise<T> | T
): Promise<T>;
export function runActorHandlerWithDeferredJoins<T, TResult>(
  handler: () => Promise<T> | T,
  afterHandler: (result: T) => Promise<TResult> | TResult,
  beforeJoins?: (result: T) => Promise<void> | void
): Promise<TResult>;
export async function runActorHandlerWithDeferredJoins<T, TResult = T>(
  handler: () => Promise<T> | T,
  afterHandler?: (result: T) => Promise<TResult> | TResult,
  beforeJoins?: (result: T) => Promise<void> | void
): Promise<TResult> {
  const scope = new ActorJoinRegistrationScope();
  let joinsPrepared = false;
  let replyPrepared = false;
  try {
    const result = await actorJoinScope.run(scope, async () => handler());
    // The registration scope ends at the handler terminal. Reply admission
    // is a separate terminal operation and must not reopen the opportunity
    // for a detached continuation to register another Join.
    scope.sealAfterHandlerTerminal();
    // Encode or otherwise validate the original reply while the Join barrier
    // is still inactive. A reply encoding failure is a handler failure and
    // must discard the registered intents before target admission starts.
    await beforeJoins?.(result);
    replyPrepared = beforeJoins !== undefined;
    // Target admission starts at the handler terminal, but its completion
    // callback remains behind the original request reply and mailbox turn.
    await scope.prepareSealedJoins();
    joinsPrepared = true;
    let afterResult!: TResult;
    let afterError: unknown;
    let afterFailed = false;
    try {
      afterResult = (afterHandler === undefined
        ? result
        : await afterHandler(result)) as unknown as TResult;
    } catch (error) {
      // A transport failure after reply encoding does not cancel a Join that
      // has already been admitted. Continue its terminal work, then rethrow
      // the transport error to the caller.
      afterError = error;
      afterFailed = true;
    }
    if (afterFailed && !replyPrepared) {
      await scope.discard();
      throw afterError;
    }
    await scope.activateSealedJoins();
    if (afterFailed) throw afterError;
    return afterResult as TResult;
  } catch (error) {
    if (!joinsPrepared) {
      await scope.discard();
    }
    throw error;
  }
}

function discardWithoutWaiting(intent: DeferredActorJoin): void {
  try {
    const discarded = intent.discard();
    if (discarded !== undefined && typeof (discarded as Promise<void>).catch === 'function') {
      void (discarded as Promise<void>).catch(() => undefined);
    }
  } catch {
    // The original registration error is the contract-visible failure. The
    // asynchronous rollback is best effort here because defer() is synchronous.
  }
}
