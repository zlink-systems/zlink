const INITIAL_RETRY_DELAY_MS = 25;
const MAX_RETRY_DELAY_MS = 1_000;

export class ZLinkActorRetryDelay {
  private delayMs = INITIAL_RETRY_DELAY_MS;

  reset(): void {
    this.delayMs = INITIAL_RETRY_DELAY_MS;
  }

  async wait(signal: AbortSignal | undefined): Promise<boolean> {
    const completed = await delayUnlessAborted(this.delayMs, signal);
    if (completed) {
      this.delayMs = Math.min(this.delayMs * 2, MAX_RETRY_DELAY_MS);
    }
    return completed;
  }
}

function delayUnlessAborted(delayMs: number, signal: AbortSignal | undefined): Promise<boolean> {
  if (signal?.aborted === true) return Promise.resolve(false);
  return new Promise((resolve) => {
    let settled = false;
    const finish = (completed: boolean): void => {
      if (settled) return;
      settled = true;
      clearTimeout(deadline);
      signal?.removeEventListener('abort', aborted);
      resolve(completed);
    };
    const aborted = (): void => finish(false);
    const deadline = setTimeout(() => finish(true), delayMs);
    deadline.unref();
    signal?.addEventListener('abort', aborted, { once: true });
    if (signal?.aborted === true) aborted();
  });
}
