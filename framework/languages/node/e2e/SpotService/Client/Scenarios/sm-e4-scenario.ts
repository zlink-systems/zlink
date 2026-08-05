// SM-E4: Timer overrun policy별 observable sequence를 확인한다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotOverrunStartRes,
  SpotOverrunStartReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

const policyNames = ['SkipLateTicks', 'CatchUpBounded', 'DelayNextTick'] as const;

export async function runSmE4(options: ClientOptions): Promise<void> {
  const policySpots = new Map<string, string>();
  for (const policy of policyNames) {
    policySpots.set(policy, `spot-sm-e4-${policy.toLowerCase()}-${Date.now().toString(36)}`);
  }

  for (const spotId of policySpots.values()) {
    const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
      spotId
    } satisfies CreateSpotReq);
    ensure(created.spotId === spotId, 'SM-E4 timer spot was not created.');
    ensure(created.nodeRid === 'play-a', 'SM-E4 timer spot was created on the wrong node.');
  }

  for (const [policy, spotId] of policySpots) {
    const started = await postJson<SpotOverrunStartRes>(options.playAUrl, '/spot/overrun/start', {
      spotId,
      name: `sm-e4-${policy}`,
      policy,
      periodMs: 25
    } satisfies SpotOverrunStartReq);
    ensure(started.started, `SM-E4 ${policy} overrun timer did not start.`);
  }

  const markers = [...policySpots].map(([policy, spotId]) =>
    `timer-overrun|rid=play-a|spot=${spotId}|name=sm-e4-${policy}`);
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: markers,
    timeoutMilliseconds: 15000
  } satisfies EvidenceWaitReq);
  ensure(
    [...policySpots].every(([policy, spotId]) =>
      evidence.filter((line) => line.includes(`timer-overrun|rid=play-a|spot=${spotId}|name=sm-e4-${policy}`)).length >= 3),
    'SM-E4 timer overrun evidence missing or incomplete.'
  );

  const skipLateTicksSkipped = evidence
    .filter((line) => line.includes('name=sm-e4-SkipLateTicks'))
    .some((line) => extractBigInt(line, 'skipped') > 0n);
  const catchUpBoundedSkipped = evidence
    .filter((line) => line.includes('name=sm-e4-CatchUpBounded'))
    .some((line) => extractBigInt(line, 'skipped') > 0n);
  const delayNextTickStayedOnSchedule = evidence
    .filter((line) => line.includes('name=sm-e4-DelayNextTick'))
    .slice(0, 3)
    .every((line) => extractBigInt(line, 'skipped') === 0n
      && extractBigInt(line, 'delivery') === extractBigInt(line, 'scheduled'));

  ensure(skipLateTicksSkipped, 'SM-E4 SkipLateTicks did not skip late ticks.');
  ensure(catchUpBoundedSkipped, 'SM-E4 CatchUpBounded did not show bounded catch-up evidence.');
  ensure(delayNextTickStayedOnSchedule, 'SM-E4 DelayNextTick did not stay on schedule.');

  console.log('scenario SM-E4 passed');
}

function extractBigInt(line: string, key: string): bigint {
  const prefix = `${key}=`;
  const start = line.indexOf(prefix);
  if (start < 0) {
    throw new Error(`Missing field '${key}' in evidence line: ${line}`);
  }
  const valueStart = start + prefix.length;
  const valueEnd = line.indexOf('|', valueStart);
  return BigInt(valueEnd < 0 ? line.slice(valueStart) : line.slice(valueStart, valueEnd));
}
