// RM-B2: Provider를 정상 종료한 뒤 target에서 제외한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { DynamicClusterLauncher } from '../Support/dynamic-cluster-launcher';
import { getJson, postJson } from '../../../http-client';
import { countNewEvidence, ensure, uniqueMarker } from '../Support/scenario-assert';
import type { ProfileRes } from '../../Shared/messages';

export async function runRmB2(options: ClientOptions): Promise<void> {
  const cluster = await DynamicClusterLauncher.start(options, 'rm-b2');
  try {
    const providerA = await cluster.startProvider('rm-b2-api-a', 'api-a');
    const providerB = await cluster.startProvider('rm-b2-api-b', 'api-b');
    const consumer = await cluster.startConsumer('rm-b2-consumer');
    await cluster.waitForProviders([providerA.channelEndpoint, providerB.channelEndpoint]);

    const beforeA = await getJson<string[]>(providerA.httpUrl, '/evidence');
    const beforeB = await getJson<string[]>(providerB.httpUrl, '/evidence');
    const markerBefore = uniqueMarker('rm-b2-before');
    const repliesBefore = await requestMany(consumer.httpUrl, markerBefore, 40);
    ensure(
      repliesBefore.some((reply) => reply.providerRid === 'api-a')
        && repliesBefore.some((reply) => reply.providerRid === 'api-b'),
      'RM-B2 expected both providers before scale-in.'
    );
    const afterA = await postJson<string[]>(providerA.httpUrl, '/evidence/wait', { contains: markerBefore });
    const afterB = await postJson<string[]>(providerB.httpUrl, '/evidence/wait', { contains: markerBefore });
    ensure(
      countNewEvidence(afterA, beforeA, 'profile-request|rid=api-a', markerBefore)
        + countNewEvidence(afterB, beforeB, 'profile-request|rid=api-b', markerBefore) === 40,
      'RM-B2 pre-scale-in evidence count did not match requests.'
    );

    const transitionMarker = uniqueMarker('rm-b2-during');
    const firstDuring = postJson<ProfileRes>(consumer.httpUrl, '/profile/request', { value: `${transitionMarker}-0` });
    const draining = cluster.drain(providerB);
    const during = [await firstDuring];
    for (let index = 1; index < 20; index += 1) {
      during.push(await postJson<ProfileRes>(consumer.httpUrl, '/profile/request', {
        value: `${transitionMarker}-${index}`
      }));
    }
    const drainResult = await draining;
    ensure(
      drainResult.outcome === 0
        && drainResult.reason === 0,
      `RM-B2 provider retire failed: ${drainResult.outcome}/${drainResult.reason}.`
    );
    ensure(
      during.every((reply) => reply.providerRid === 'api-a' || reply.providerRid === 'api-b'),
      'RM-B2 target-free request failed during graceful scale-in.'
    );

    await postJson(consumer.httpUrl, '/locations/peers/wait', {
      rid: 'api-b', present: false, timeoutMilliseconds: 30_000
    });
    await cluster.waitForSingleProvider('api-a', providerA.channelEndpoint);
    const markerAfter = uniqueMarker('rm-b2-after');
    const repliesAfter = await requestMany(consumer.httpUrl, markerAfter, 20);
    ensure(repliesAfter.every((reply) => reply.providerRid === 'api-a'), 'RM-B2 after scale-in should reach api-a only.');
    console.log('scenario RM-B2 passed');
  } finally {
    await cluster.close();
  }
}

async function requestMany(consumerUrl: string, marker: string, count: number): Promise<ProfileRes[]> {
  const replies: ProfileRes[] = [];
  for (let index = 0; index < count; index += 1) {
    replies.push(await postJson<ProfileRes>(consumerUrl, '/profile/request', { value: `${marker}-${index}` }));
  }
  return replies;
}
