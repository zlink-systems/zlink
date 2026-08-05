// RL-D5: Fixed soak 중 lifecycle checkpoints를 통과한다 시나리오를 검증한다.
import { randomUUID } from 'node:crypto';
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { profileReq } from '../Support/resilience-helpers';
import { findProviderEvidenceMarkers } from '../Support/provider-evidence';
import { ensure } from '../Support/scenario-assert';

interface LatencySample {
  readonly elapsedMs: number;
  readonly latencyMs: number;
}

interface WorkerResult {
  readonly requests: number;
  readonly sends: number;
  readonly samples: readonly LatencySample[];
}

const workloadIntervalMs = 20;

export async function runRlD5(options: ClientOptions): Promise<void> {
  ensure(options.consumerUrls.length >= 8, 'RL-D5 requires at least eight concurrent clients.');
  const runId = randomUUID().replaceAll('-', '');
  const startedAt = Date.now();
  const deadline = startedAt + options.soakDurationMs;
  const results = await Promise.all(options.consumerUrls.map((url, index) =>
    runWorker(url, index + 1, runId, startedAt, deadline)));
  const elapsedMs = Date.now() - startedAt;
  ensure(elapsedMs >= options.soakDurationMs, 'RL-D5 workload ended before the configured soak duration.');
  ensure(results.every((result) => result.requests >= 20), 'RL-D5 a client did not sustain request traffic.');
  ensure(results.every((result) => result.sends === result.requests), 'RL-D5 request/send mix became unbalanced.');

  const samples = results.flatMap((result) => result.samples);
  const firstHalfP95 = percentile(
    samples.filter((sample) => sample.elapsedMs < options.soakDurationMs / 2).map((sample) => sample.latencyMs),
    0.95
  );
  const secondHalfP95 = percentile(
    samples.filter((sample) => sample.elapsedMs >= options.soakDurationMs / 2).map((sample) => sample.latencyMs),
    0.95
  );
  ensure(firstHalfP95 >= 0 && secondHalfP95 >= 0, 'RL-D5 did not collect latency samples across both halves.');
  ensure(
    secondHalfP95 <= Math.max(firstHalfP95 * 2.5, firstHalfP95 + 100),
    `RL-D5 latency drifted: first-p95=${firstHalfP95}ms second-p95=${secondHalfP95}ms.`
  );

  await confirmTailSends(options, runId);

  const cleanupReplies = await Promise.all(options.consumerUrls.map((url, index) =>
    postJson<ProfileRes>(url, '/profile/request/new-client', profileReq(`rl-d5-cleanup-${runId}-${index}`))));
  ensure(cleanupReplies.every((reply) => reply.value === 'profile:fast'), 'RL-D5 short-lived client cleanup failed.');
  const followUps = await Promise.all(options.consumerUrls.map((url, index) =>
    postJson<ProfileRes>(url, '/profile/request', profileReq(`rl-d5-follow-up-${runId}-${index}`))));
  ensure(followUps.every((reply) => reply.value === 'profile:fast'), 'RL-D5 post-soak follow-up failed.');

  console.log(
    `scenario RL-D5 passed durationMs=${elapsedMs} requests=${results.reduce((sum, item) => sum + item.requests, 0)}`
    + ` sends=${results.reduce((sum, item) => sum + item.sends, 0)}`
    + ` firstHalfP95Ms=${firstHalfP95} secondHalfP95Ms=${secondHalfP95}`
  );
}

async function runWorker(
  consumerUrl: string,
  workerId: number,
  runId: string,
  startedAt: number,
  deadline: number
): Promise<WorkerResult> {
  const samples: LatencySample[] = [];
  let sequence = 0;
  while (Date.now() < deadline) {
    sequence += 1;
    const requestStarted = Date.now();
    const reply = await postJson<ProfileRes>(
      consumerUrl,
      '/profile/request',
      profileReq(`rl-d5-${runId}-w${workerId}-req${sequence}`)
    );
    ensure(reply.value === 'profile:fast', `RL-D5 worker ${workerId} received an invalid reply.`);
    samples.push({ elapsedMs: requestStarted - startedAt, latencyMs: Date.now() - requestStarted });
    await postJson(consumerUrl, '/profile/command', {
      commandId: `rl-d5-${runId}-w${workerId}-cmd${sequence}`
    });
    await delay(workloadIntervalMs);
  }
  return {
    requests: sequence,
    sends: sequence,
    samples
  };
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function confirmTailSends(options: ClientOptions, runId: string): Promise<void> {
  const pending = new Map(options.consumerUrls.map((url, index) => [
    `rl-d5-${runId}-w${index + 1}-tail`,
    url
  ]));
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline && pending.size > 0) {
    await Promise.all([...pending].map(([commandId, url]) =>
      postJson(url, '/profile/command', { commandId })));
    await new Promise((resolve) => setTimeout(resolve, 100));
    const found = findProviderEvidenceMarkers(options, new Set(pending.keys()));
    for (const commandId of found) pending.delete(commandId);
  }
  ensure(pending.size === 0, `RL-D5 tail send unavailable for clients: ${[...pending.values()].join(', ')}`);
}

function percentile(values: readonly number[], quantile: number): number {
  if (values.length === 0) return -1;
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * quantile) - 1)];
}
