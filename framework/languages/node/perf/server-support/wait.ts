// Public status is polled during setup; a probe call itself is never retried (perf §21).
export const sleep = (ms: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, ms));

export async function until(
  condition: () => boolean | Promise<boolean>,
  timeoutMs: number,
  what: string,
  intervalMs = 10
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!(await condition())) {
    if (Date.now() >= deadline)
      throw new Error(`Timed out after ${timeoutMs} ms waiting for ${what}.`);
    await sleep(intervalMs);
  }
}

// Request streams submit continuously without waiting for earlier replies; one sweep yields to the event loop.
export async function runRequestStreams(
  streams: number,
  canIssue: () => boolean,
  issue: (stream: number) => Promise<void>,
  onError: (error: unknown) => void
): Promise<void> {
  if (streams <= 0) return;
  let stream = 0;
  while (canIssue()) {
    void issue(stream).catch(onError);
    if (++stream === streams) {
      stream = 0;
      await new Promise<void>((resolve) => setImmediate(resolve));
    }
  }
}

// Each stream advances after the supplied call terminal: local reply or remote one-way admission.
export function runTerminalStreams(
  streams: number,
  canIssue: () => boolean,
  submit: (stream: number) => Promise<boolean | void>
): Promise<void> {
  return Promise.all(
    Array.from({ length: streams }, async (_, stream) => {
      while (canIssue()) {
        if ((await submit(stream)) === false) return;
      }
    })
  ).then(() => undefined);
}
