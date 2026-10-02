import { awaitWithAbort } from '../abort';
import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';

export function remainingActorRequestTimeout(
  actorId: string,
  deadlineMs: number | undefined
): number | undefined {
  if (deadlineMs === undefined) return undefined;
  const remaining = deadlineMs - performance.now();
  if (remaining <= 0) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      `Actor request '${actorId}' exceeded its deadline.`
    );
  }
  return Math.max(1, Math.ceil(remaining));
}

/** Only actual Store, delivery-gate, and handoff waits acquire a deadline handle. */
export async function waitActorReply<T>(
  reply: Promise<T>,
  actorId: string,
  deadlineMs: number | undefined,
  signal?: AbortSignal
): Promise<T> {
  if (deadlineMs === undefined) return await awaitWithAbort(reply, signal);
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<never>((_resolve, reject) => {
    const remaining = remainingActorRequestTimeout(actorId, deadlineMs);
    timer = setTimeout(
      () =>
        reject(
          createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `Actor request '${actorId}' exceeded its deadline.`
          )
        ),
      remaining
    );
  });
  try {
    return await awaitWithAbort(Promise.race([reply, timeout]), signal);
  } finally {
    clearTimeout(timer);
  }
}
