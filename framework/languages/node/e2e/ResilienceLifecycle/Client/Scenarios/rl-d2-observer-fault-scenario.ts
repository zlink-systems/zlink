// RL-D2: Observer failure를 messaging에서 격리한다 시나리오를 검증한다.
import type { ProfileRes, RequestFailureRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { readProviderEvidence } from '../Support/provider-evidence';
import { profileReq, waitForAnyProviderTraffic } from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';

export async function runRlD2(options: ClientOptions): Promise<void> {
  await waitForAnyProviderTraffic(options.consumerUrl, 'rl-d2-ready');
  const baselineRuntimeErrors = countRuntimeErrors(await readProviderEvidence(options));

  await postJson(options.providerAUrl, '/admin/fault/observer-throws');
  await postJson(options.providerBUrl, '/admin/fault/observer-throws');

  const missing = await postJson<RequestFailureRes>(
    options.consumerUrl,
    '/profile/missing-request',
    profileReq('rl-d2-error')
  );
  ensure(missing.failed, 'RL-D2 missing handler request should fail.');
  ensure(
    missing.failureMessage.includes('request handler is registered'),
    `RL-D2 request did not reach the remote no-handler path: ${missing.failureMessage}`
  );

  await waitForEitherEvidence(options, 'dispatch-error', 'RL-D2 did not record dispatch-error evidence.');
  const runtimeError = await waitForRuntimeError(options, baselineRuntimeErrors);
  ensure(
    runtimeError.includes('event_id=zlink.runtime_error')
      && runtimeError.includes('kind=observer_failed')
      && runtimeError.includes('source=message_flow_observer')
      && runtimeError.includes('fields=eventId,kind,reason,source,timestamp'),
    `RL-D2 runtime error sink event did not preserve the public contract: ${runtimeError}`
  );

  const followUp = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request',
    profileReq('rl-d2-after')
  );
  ensure(followUp.value === 'profile:fast', 'RL-D2 messaging did not continue after observer failure.');

  await postJson(options.providerAUrl, '/admin/fault/none');
  await postJson(options.providerBUrl, '/admin/fault/none');

  await waitForEitherEvidence(options, 'marker=rl-d2-after', 'RL-D2 did not record expected follow-up evidence.');
  await new Promise((resolve) => setTimeout(resolve, 500));
  ensure(
    countRuntimeErrors(await readProviderEvidence(options)) === baselineRuntimeErrors + 1,
    'RL-D2 runtime error sink did not receive exactly one observer failure event.'
  );

  console.log('scenario RL-D2 passed');
}

function countRuntimeErrors(evidence: readonly string[]): number {
  return evidence.filter((line) => line.startsWith('runtime-error|')).length;
}

async function waitForRuntimeError(
  options: ClientOptions,
  baseline: number
): Promise<string> {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    const evidence = await readProviderEvidence(options);
    const runtimeErrors = evidence.filter((line) => line.startsWith('runtime-error|'));
    if (runtimeErrors.length === baseline + 1) {
      return runtimeErrors.at(-1)!;
    }
    ensure(
      runtimeErrors.length <= baseline + 1,
      'RL-D2 runtime error sink received more than one observer failure event.'
    );
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  ensure(false, 'RL-D2 runtime error sink event timed out.');
}

async function waitForEitherEvidence(options: ClientOptions, contains: string, message: string): Promise<void> {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    const snapshots = await Promise.allSettled([
      getJson<string[]>(options.providerAUrl, '/evidence'),
      getJson<string[]>(options.providerBUrl, '/evidence')
    ]);
    if (snapshots.some((snapshot) =>
      snapshot.status === 'fulfilled' && snapshot.value.some((line) => line.includes(contains)))) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  ensure(false, message);
}
