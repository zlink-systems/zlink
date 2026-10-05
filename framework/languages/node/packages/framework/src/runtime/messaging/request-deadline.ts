import { awaitWithAbort } from '../abort';
import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';

export function remainingRequestTimeout(
  operation: string,
  deadlineMs: number | undefined
): number | undefined {
  if (deadlineMs === undefined) return undefined;
  const remaining = deadlineMs - performance.now();
  if (remaining <= 0) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      `${operation} exceeded its deadline.`
    );
  }
  return Math.max(1, Math.ceil(remaining));
}

/** Bounds caller waiting across outbound admission and reply; late completions stay observed. */
export async function waitRequestReply<T>(
  reply: Promise<T>,
  operation: string,
  deadlineMs: number | undefined,
  signal?: AbortSignal,
  endWait?: () => void,
  discardLateReply?: (reply: T) => void
): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  let waiting = reply;
  if (deadlineMs !== undefined) {
    const timeout = new Promise<never>((_resolve, reject) => {
      const remaining = remainingRequestTimeout(operation, deadlineMs);
      timer = setTimeout(
        () =>
          reject(
            createInternalFrameworkException(
              ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
              `${operation} exceeded its deadline.`
            )
          ),
        remaining
      );
    });
    waiting = Promise.race([reply, timeout]);
  }
  try {
    return await awaitWithAbort(waiting, signal);
  } catch (error) {
    endWait?.();
    if (discardLateReply !== undefined) void reply.then(discardLateReply, () => undefined);
    throw error;
  } finally {
    clearTimeout(timer);
  }
}
