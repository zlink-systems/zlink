import { createWriteStream, writeFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
import { finished } from 'node:stream/promises';
import type { Readable } from 'node:stream';
import { inspect } from 'node:util';

export interface Endpoints {
  stream: string;
  api?: string;
  worlds?: Record<string, string>;
}

export interface Foreground {
  signal: AbortSignal;
  publish: (endpoints: Endpoints) => void;
}

export function scenarioStart(name: string, selected: readonly string[]): void {
  console.log(`━━ ${name} (${selected.indexOf(name) + 1}/${selected.length}) ━━`);
}

export function reportFailure(label: string, error: unknown, logDir: string, file: string): void {
  const path = `${logDir}/${file}-failure.log`;
  writeFileSync(path, inspect(error, { depth: null }) + '\n');
  const message = (error instanceof Error ? error.message : String(error)).split(/\r?\n/)[0];
  console.error(`${label}: ${message} (${path})`);
}

export function attachedScenarios(
  selected: readonly string[],
  isolated: readonly string[],
  reason = 'these scenarios replace nodes and require an isolated run.'
): string[] {
  const skipped = selected.filter((name) => isolated.includes(name));
  if (skipped.length) console.log(`Skipping ${skipped.join(', ')}: ${reason}`);
  return selected.filter((name) => !isolated.includes(name));
}

export async function keepRunning(
  foreground: Foreground,
  endpoints: Endpoints,
  logDir: string,
  exits: Promise<Error>[]
): Promise<void> {
  if (foreground.signal.aborted) return;
  foreground.publish(endpoints);
  for (const endpoint of Object.values(endpoints.worlds ?? { session: endpoints.stream }))
    console.log(`Server ready: ${endpoint} (game clients connect here)`);
  if (endpoints.api) console.log(`API ready: ${endpoints.api}`);
  console.log(`Logs: ${logDir}`);
  let stop = (): void => {};
  const interrupted = new Promise<undefined>((resolve) => {
    stop = () => resolve(undefined);
    foreground.signal.addEventListener('abort', stop, { once: true });
  });
  try {
    const error = await Promise.race([interrupted, ...exits]);
    if (error) throw error;
  } finally {
    foreground.signal.removeEventListener('abort', stop);
  }
}

export async function captureMessageFlow(
  stderr: Readable,
  path: string,
  node: string,
  foreground: boolean
): Promise<void> {
  const logs = createWriteStream(path);
  const reader = createInterface({ input: stderr, crlfDelay: Infinity });
  try {
    for await (const line of reader) {
      const prefix = 'ZLINK_MESSAGE_FLOW ';
      if (!line.startsWith(prefix)) continue;
      const json = line.slice(prefix.length);
      logs.write(json + '\n');
      if (!foreground) continue;
      const event = JSON.parse(json) as { packet_name?: string; phase?: string; surface?: string };
      if (
        event.surface === 'stream' &&
        event.phase === 'completed' &&
        event.packet_name &&
        /^(Join|Enter|ChangeChannel|CreateParty|OpenDungeon|Queue|Shoot|Advance)/.test(
          event.packet_name
        )
      )
        console.log(`[${node}] ${event.packet_name} completed`);
    }
  } finally {
    logs.end();
    await finished(logs);
  }
}
