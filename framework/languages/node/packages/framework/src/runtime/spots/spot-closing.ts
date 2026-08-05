import {
  ZLinkSpotCloseReason,
  type ZLinkSpotClosingContext
} from '../../contracts';

const DEFAULT_CLEANUP_TIMEOUT_MS = 30_000;

export async function invokeSpotClosing(
  callback: ((context: ZLinkSpotClosingContext, cleanupSignal: AbortSignal) => Promise<void>) | undefined,
  reason: ZLinkSpotCloseReason,
  timeoutMs = DEFAULT_CLEANUP_TIMEOUT_MS
): Promise<void> {
  if (callback === undefined) return;
  const deadline = new Date(Date.now() + timeoutMs);
  const controller = new AbortController();
  let timer: ReturnType<typeof setTimeout> | undefined;
  const expired = new Promise<void>(resolve => {
    timer = setTimeout(() => {
      controller.abort();
      resolve();
    }, timeoutMs);
  });
  const operation = callback({ reason, deadline }, controller.signal);
  try {
    await Promise.race([operation, expired]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
    void operation.catch(() => undefined);
  }
}
