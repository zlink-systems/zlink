import assert from 'node:assert/strict';
import { getJson, postJson } from '../../../http-client';
import type { ClientOptions } from './client-options';

export { assert };
export { getJson, postJson };

export async function waitForEvidence(baseUrl: string, contains: string, timeoutMilliseconds = 10_000): Promise<readonly string[]> {
  return await postJson<readonly string[]>(baseUrl, '/evidence/wait', { contains, timeoutMilliseconds });
}

export async function waitFor<T>(read: () => Promise<T>, predicate: (value: T) => boolean, label: string, timeoutMs = 10_000): Promise<T> {
  const deadline = Date.now() + timeoutMs;
  let last: T | undefined;
  while (Date.now() < deadline) {
    try {
      last = await read();
      if (predicate(last)) return last;
    } catch {
      // The next bounded poll is the readiness boundary.
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`${label}: ${JSON.stringify(last)}`);
}

export function routeOptions(options: ClientOptions): ClientOptions {
  return options;
}

export function ids(entries: readonly string[], marker: string): readonly string[] {
  return entries.filter((entry) => entry.includes(marker));
}
