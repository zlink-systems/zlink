export function ensure(condition: boolean, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

export async function eventually(
  condition: () => Promise<boolean>,
  failureMessage: string,
  timeoutMs = 20_000
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await condition()) {
      return;
    }
    await delay(100);
  }
  throw new Error(failureMessage);
}

export function isConnectionFailure(error: unknown): boolean {
  if (!(error instanceof Error)) {
    return false;
  }
  const message = error.message.toLowerCase();
  return message.includes('fetch failed')
    || message.includes('econnrefused')
    || message.includes('socket')
    || message.includes('terminated');
}

export function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
