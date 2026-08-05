// RL-D1: High fanout에서 subscriber를 서로 격리한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

const eventCount = 120;

export async function runRlD1(options: ClientOptions): Promise<void> {
  ensure(options.consumerUrls.length >= 8, 'RL-D1 requires at least eight fanout subscribers.');
  const warmupRun = `warmup-${randomUUID().replaceAll('-', '')}`;
  for (let sequence = 1; sequence <= 40; sequence += 1) {
    await publish(options.providerAUrl, warmupRun, sequence);
  }
  await Promise.all(options.consumerUrls.map((url) => postJson<string[]>(url, '/evidence/wait', {
    contains: `run=${warmupRun}|`,
    timeoutMilliseconds: 10_000
  })));
  await Promise.all(options.consumerUrls.map((url) => postJson(url, '/evidence/clear', {})));

  const runId = randomUUID().replaceAll('-', '');
  for (let sequence = 1; sequence <= eventCount; sequence += 1) {
    await publish(options.providerAUrl, runId, sequence);
  }
  await Promise.all(options.consumerUrls.map((url) => postJson<string[]>(url, '/evidence/wait', {
    contains: `run=${runId}|seq=${eventCount}|`,
    timeoutMilliseconds: 20_000
  })));

  for (const subscriber of options.consumerUrls) {
    const evidence = await getJson<string[]>(subscriber, '/evidence');
    const sequences = evidence
      .filter((line) => line.includes(`load-event|`) && line.includes(`run=${runId}|`))
      .map((line) => Number.parseInt(line.match(/\|seq=(\d+)\|/)?.[1] ?? '0', 10));
    ensure(sequences.length === eventCount, `RL-D1 subscriber ${subscriber} did not receive every event.`);
    ensure(
      sequences.every((sequence, index) => sequence === index + 1),
      `RL-D1 subscriber ${subscriber} observed a missing, duplicate, or reordered event.`
    );
  }

  console.log('scenario RL-D1 passed');
}

async function publish(providerUrl: string, runId: string, sequence: number): Promise<void> {
  await postJson(providerUrl, '/fanout/publish', { runId, sequence });
}
