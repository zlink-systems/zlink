import type { ZLinkHttpClient } from '@zlink-systems/http-client';
import { delay, post, require } from './scenario-support.js';

interface RelocationResult {
  readonly outcome: number;
  readonly reason: number;
}

export interface MetricEvidence {
  readonly name: string;
  readonly kind: 'counter' | 'updown' | 'histogram';
  readonly value: number;
  readonly unit: string;
  readonly tags: Readonly<Record<string, string | number | boolean>>;
}

export interface DrainStatus {
  readonly ready: boolean;
  readonly result?: RelocationResult;
  readonly peerRows?: readonly { readonly nodeRid: string; readonly draining: boolean; readonly generation: string }[];
  readonly actors?: readonly { readonly actorId: string; readonly nodeRid: string; readonly generation: string }[];
}

export function retireCompleted(status: DrainStatus): boolean {
  return status.result?.outcome === 0 && status.result.reason === 0;
}

export function retireForceStopped(status: DrainStatus): boolean {
  return status.result?.outcome === 1;
}

export async function metrics(client: ZLinkHttpClient): Promise<readonly MetricEvidence[]> {
  return await client.get('/metrics').fetch<MetricEvidence[]>();
}

export function metric(
  values: readonly MetricEvidence[],
  name: string,
  predicate: (value: MetricEvidence) => boolean = () => true
): MetricEvidence {
  const found = values.find((value) => value.name === name && predicate(value));
  require(found !== undefined, `Metric '${name}' was not recorded.`);
  return found;
}

export async function startDrain(client: ZLinkHttpClient, deadlineMs: number): Promise<void> {
  await post(client, '/drain', { deadlineMs });
}

export async function waitForDrain(
  client: ZLinkHttpClient,
  predicate: (status: DrainStatus) => boolean,
  message: string,
  timeoutMs = 3000
): Promise<DrainStatus> {
  return await waitFor(async () => await client.get('/drain/status').fetch<DrainStatus>(), predicate, message, timeoutMs);
}

export async function waitFor<T>(
  read: () => Promise<T>,
  predicate: (value: T) => boolean,
  message: string,
  timeoutMs = 3000
): Promise<T> {
  const deadline = Date.now() + timeoutMs;
  let last: T | undefined;
  while (Date.now() < deadline) {
    last = await read();
    if (predicate(last)) return last;
    await delay(50);
  }
  throw new Error(`${message}; last=${JSON.stringify(last)}`);
}

export async function waitForFlow(
  clients: readonly ZLinkHttpClient[],
  packetName: string,
  timeoutMs = 3000
): Promise<string> {
  const uuid = /flow=([0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})/;
  return await waitFor(async () => {
    const sets = await Promise.all(clients.map(async (client) => {
      const log = await readFlowLog(client);
      return new Set(log.split('\n')
        .filter((line) => line.includes(`packet=${packetName} `))
        .map((line) => line.match(uuid)?.[1])
        .filter((value): value is string => value !== undefined));
    }));
    const first = sets[0] ?? new Set<string>();
    return [...first].find((value) => sets.every((set) => set.has(value))) ?? '';
  }, (value) => value.length > 0, `No shared flow for packet '${packetName}' across roles`, timeoutMs);
}

export async function readFlowLog(client: ZLinkHttpClient): Promise<string> {
  return (await client.get('/flow-log').fetch<{ readonly content: string }>()).content;
}
