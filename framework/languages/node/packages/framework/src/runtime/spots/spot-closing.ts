import { createDeadlineExceededError, isAbortError, isDeadlineExceededError } from '../abort';
import { ZLinkSpotCloseReason, type ZLinkSpotClosingContext } from '../../contracts';

const DEFAULT_CLEANUP_TIMEOUT_MS = 30_000;

export async function invokeSpotClosing(
  callback:
    ((context: ZLinkSpotClosingContext, cleanupSignal: AbortSignal) => Promise<void>) | undefined,
  reason: ZLinkSpotCloseReason,
  cleanupDeadline: Date | number = DEFAULT_CLEANUP_TIMEOUT_MS
): Promise<void> {
  if (callback === undefined) return;
  const deadline = new Date(
    cleanupDeadline instanceof Date ? cleanupDeadline.getTime() : Date.now() + cleanupDeadline
  );
  const timeoutMs = Math.max(
    0,
    cleanupDeadline instanceof Date ? cleanupDeadline.getTime() - Date.now() : cleanupDeadline
  );
  const controller = new AbortController();
  let timer: ReturnType<typeof setTimeout> | undefined;
  const expired = new Promise<void>((resolve, reject) => {
    timer = setTimeout(() => {
      controller.abort();
      if (reason === ZLinkSpotCloseReason.HostShutdown)
        reject(createDeadlineExceededError('Spot closing exceeded the host teardown end time.'));
      else resolve();
    }, timeoutMs);
  });
  try {
    const operation = Promise.resolve()
      .then(() => callback({ reason, deadline }, controller.signal))
      .catch((error) => {
        if (controller.signal.aborted && isAbortError(error)) {
          throw createDeadlineExceededError('Spot closing exceeded the host teardown end time.');
        }
        if (
          reason === ZLinkSpotCloseReason.HostShutdown &&
          (isAbortError(error) || isDeadlineExceededError(error))
        ) {
          throw new ZLinkSpotClosingFailure(error);
        }
        throw error;
      });
    await Promise.race([operation, expired]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

class ZLinkSpotClosingFailure extends Error {
  constructor(cause: unknown) {
    super('Spot closing callback failed.', { cause });
  }
}
