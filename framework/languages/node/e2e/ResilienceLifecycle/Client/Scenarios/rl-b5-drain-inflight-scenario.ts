// RL-B5: Weight 변경 전에 accepted된 request를 완료한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runRlB5(options: ClientOptions): Promise<void> {
  await postJson(options.providerAUrl, '/admin/restore');
  await postJson(options.providerBUrl, '/admin/restore');
  await waitForWeight(options.providerAUrl, 100);
  await waitForWeight(options.providerBUrl, 100);

  const slowMarker = `rl-b5-slow-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const slowTask = postJson<ProfileRes>(options.consumerUrl, '/profile/request/no-retry', {
    value: 'slow',
    marker: slowMarker
  });

  const slowProvider = await waitForSlowStart(options.providerAUrl, options.providerBUrl, slowMarker);
  const drainedProviderUrl = slowProvider === 'api-a' ? options.providerAUrl : options.providerBUrl;
  const healthyProvider = slowProvider === 'api-a' ? 'api-b' : 'api-a';
  const beforeDrain = await getJson<string[]>(drainedProviderUrl, '/evidence');

  await postJson(drainedProviderUrl, '/admin/drain');
  await waitForWeight(drainedProviderUrl, 0);

  for (let i = 0; i < 12; i += 1) {
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', {
      value: 'fast',
      marker: `rl-b5-drained-${i}`
    });
    ensure(reply.providerRid === healthyProvider, 'RL-B5 drain did not block new requests to the drained provider.');
  }

  const slowReply = await slowTask;
  ensure(
    slowReply.providerRid === slowProvider,
    'RL-B5 in-flight slow reply did not complete on the drained provider.'
  );

  const afterDrain = await getJson<string[]>(drainedProviderUrl, '/evidence');
  ensure(
    countMatching(afterDrain, beforeDrain, `profile-request|rid=${slowProvider}|value=fast|marker=rl-b5-drained-`) === 0,
    'RL-B5 drained provider accepted new requests after drain.'
  );
  ensure(
    afterDrain.some((line) => line.includes(`profile-request|rid=${slowProvider}|value=slow|marker=${slowMarker}`)),
    'RL-B5 in-flight completion evidence missing.'
  );

  await postJson(drainedProviderUrl, '/admin/restore');
  await waitForWeight(drainedProviderUrl, 100);

  for (let i = 0; i < 40; i += 1) {
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', {
      value: 'fast',
      marker: `rl-b5-after-${i}`
    });
    ensure(reply.value === 'profile:fast', 'RL-B5 restored request returned an unexpected value.');
  }

  await postJson<string[]>(drainedProviderUrl, '/evidence/wait', {
    contains: `profile-request|rid=${slowProvider}|value=fast|marker=rl-b5-after-`
  });
  console.log('scenario RL-B5 passed');
}

async function waitForWeight(providerUrl: string, expected: number): Promise<void> {
  await postJson(providerUrl, '/admin/weight/wait', { expected });
}

async function waitForSlowStart(providerAUrl: string, providerBUrl: string, marker: string): Promise<string> {
  const waitA = postJson<string[]>(providerAUrl, '/evidence/wait', {
    contains: `profile-start|rid=api-a|value=slow|marker=${marker}`,
    timeoutMilliseconds: 15000
  });
  const waitB = postJson<string[]>(providerBUrl, '/evidence/wait', {
    contains: `profile-start|rid=api-b|value=slow|marker=${marker}`,
    timeoutMilliseconds: 15000
  });
  const completed = await Promise.race([
    waitA.then(() => 'api-a' as const),
    waitB.then(() => 'api-b' as const)
  ]);
  return completed;
}

function countMatching(after: readonly string[], before: readonly string[], pattern: string): number {
  const beforeCount = before.filter((line) => line.includes(pattern)).length;
  const afterCount = after.filter((line) => line.includes(pattern)).length;
  return Math.max(0, afterCount - beforeCount);
}
