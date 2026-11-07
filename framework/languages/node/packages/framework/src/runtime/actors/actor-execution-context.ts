import { AsyncLocalStorage } from 'node:async_hooks';

interface ZLinkActorExecutionContext {
  readonly actorId: string;
  readonly spotId?: unknown;
  active: boolean;
}

const actorExecutionStorage = new AsyncLocalStorage<ZLinkActorExecutionContext>();
const hiddenActiveProperty = Object.freeze({ enumerable: false });

/** Runs one Actor handler while retaining its Actor FIFO claim across Yield. */
export function runZLinkActorExecution<T>(
  actorId: string,
  spotId: unknown,
  operation: () => Promise<T> | T
): Promise<T> {
  const context: ZLinkActorExecutionContext = { actorId, spotId, active: true };
  Object.defineProperty(context, 'active', hiddenActiveProperty);
  return actorExecutionStorage.run(context, async () => {
    try {
      return await operation();
    } finally {
      // Detached async work can inherit AsyncLocalStorage after the handler
      // terminal. The shared object prevents that stale work from claiming it
      // still owns the Actor FIFO.
      context.active = false;
    }
  });
}

export function currentZLinkActorExecution():
  | Readonly<{
      actorId: string;
      spotId?: unknown;
    }>
  | undefined {
  const context = actorExecutionStorage.getStore();
  return context?.active === true ? context : undefined;
}

/** Runs Spot work outside the submitting Actor's execution scope. */
export function runOutsideZLinkActorExecution<T>(operation: () => T): T {
  return actorExecutionStorage.exit(operation);
}
