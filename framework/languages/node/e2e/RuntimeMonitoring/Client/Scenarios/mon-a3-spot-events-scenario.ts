// MON-A3: Channel readiness와 실제 request 결과를 대조한다 시나리오를 검증한다.
import type { EvidenceWaitReq, ProfileReq, ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { waitForRouteStatus, delay } from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

export async function runMonA3(options: ClientOptions): Promise<void> {
  const ready = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.channels.some((channel) =>
      channel.channelName === 'monitor.profile' && channel.isReady && channel.readyTargetCount === 1),
    'MON-A3 expected svc-b to be the only ready profile target.'
  );
  const first = await requestProfile(options, 'mon-a3-before-exclude');
  ensure(first.providerRid === 'svc-b' && first.value === 'profile:before-exclude', 'MON-A3 initial request did not reach svc-b.');

  await postJson<object>(options.serviceBUrl, '/admin/exclude', {});
  await waitForWeight(options.serviceBUrl, 0);
  const excluded = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.channels.some((channel) =>
      channel.channelName === 'monitor.profile' && !channel.isReady && channel.readyTargetCount === 0),
    'MON-A3 Channel status did not remove the excluded target.'
  );
  ensure(BigInt(excluded.sequence) > BigInt(ready.sequence), 'MON-A3 Channel readiness sequence did not advance after weight exclusion.');

  let rejected = false;
  try {
    await requestProfile(options, 'mon-a3-excluded');
  } catch {
    rejected = true;
  }
  ensure(rejected, 'MON-A3 request succeeded while no positive-weight target was ready.');

  await postJson<object>(options.serviceBUrl, '/admin/include', {});
  await waitForWeight(options.serviceBUrl, 100);
  const restored = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.channels.some((channel) =>
      channel.channelName === 'monitor.profile' && channel.isReady && channel.readyTargetCount === 1),
    'MON-A3 Channel status did not restore the included target.'
  );
  const final = await requestProfile(options, 'mon-a3-after-include');
  ensure(final.providerRid === 'svc-b' && final.value === 'profile:after-include', 'MON-A3 restored request did not reach svc-b.');
  ensure(BigInt(restored.sequence) > BigInt(excluded.sequence), 'MON-A3 Channel sequence did not advance after weight restoration.');

  await postJson<string[]>(options.serviceBUrl, '/evidence/wait', {
    containsAll: ['profile-request|rid=svc-b|marker=mon-a3-after-include|value=after-include'],
    containsAnyGroups: [],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  console.log('scenario MON-A3 passed');
}

async function requestProfile(options: ClientOptions, marker: string): Promise<ProfileRes> {
  const request: ProfileReq = { value: marker.replace('mon-a3-', ''), marker };
  return await postJson<ProfileRes>(options.triggerUrl, '/profile/request/service-b', request);
}

async function waitForWeight(serviceUrl: string, expected: number): Promise<void> {
  const deadline = Date.now() + 10_000;
  while (Date.now() < deadline) {
    if ((await getJson<{ readonly weight: number }>(serviceUrl, '/admin/weight')).weight === expected) return;
    await delay();
  }
  throw new Error(`MON-A3 service weight did not become ${expected}.`);
}
