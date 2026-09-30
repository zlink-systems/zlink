// Public status is polled during setup; a probe call itself is never retried (perf §21).
export const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

export async function until(condition: () => boolean | Promise<boolean>, timeoutMs: number, what: string, intervalMs = 10): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!(await condition())) {
    if (Date.now() >= deadline) throw new Error(`Timed out after ${timeoutMs} ms waiting for ${what}.`);
    await sleep(intervalMs);
  }
}

// The measured loop shape shared by every server-driven source: `streams` logical streams, `inflight` loops each.
export function runLoops(streams: number, inflight: number, loop: (stream: number) => Promise<void>): Promise<void> {
  return Promise.all(Array.from({ length: streams }, (_, stream) => Array.from({ length: inflight }, () => loop(stream))).flat()).then(() => undefined);
}
