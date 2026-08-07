// RL-D2: Telemetry provider failure를 messaging에서 격리한다 시나리오를 검증한다.
import type { ProfileRes, RequestFailureRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { readProviderEvidence } from '../Support/provider-evidence';
import { profileReq, waitForAnyProviderTraffic } from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';

export async function runRlD2(options: ClientOptions): Promise<void> {
  await waitForAnyProviderTraffic(options.consumerUrl, 'rl-d2-ready');
  const baselineProviderFailures = countProviderFailures(await readProviderEvidence(options));

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
  await waitForProviderFailure(options, baselineProviderFailures);

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
    countProviderFailures(await readProviderEvidence(options)) === baselineProviderFailures + 1,
    'RL-D2 logger provider did not report exactly one injected failure.'
  );

  console.log('scenario RL-D2 passed');
}

function countProviderFailures(evidence: readonly string[]): number {
  return evidence.filter((line) => line.startsWith('telemetry-provider-failure|')).length;
}

async function waitForProviderFailure(
  options: ClientOptions,
  baseline: number
): Promise<void> {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    const evidence = await readProviderEvidence(options);
    const failures = evidence.filter((line) => line.startsWith('telemetry-provider-failure|'));
    if (failures.length === baseline + 1) {
      return;
    }
    ensure(
      failures.length <= baseline + 1,
      'RL-D2 logger provider received more than one injected failure.'
    );
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  ensure(false, 'RL-D2 logger provider failure evidence timed out.');
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
