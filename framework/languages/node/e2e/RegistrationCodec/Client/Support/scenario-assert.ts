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
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(failureMessage);
}
