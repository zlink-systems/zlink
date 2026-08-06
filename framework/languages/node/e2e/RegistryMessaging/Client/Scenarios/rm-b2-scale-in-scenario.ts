// RM-B2: Provider를 정상 종료한 뒤 target에서 제외한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { DynamicClusterLauncher } from '../Support/dynamic-cluster-launcher';
import { postJson } from '../../../http-client';
import { ensure, uniqueMarker } from '../Support/scenario-assert';
import type { ProfileRes } from '../../Shared/messages';

export async function runRmB2(options: ClientOptions): Promise<void> {
  const cluster = await DynamicClusterLauncher.start(options, 'rm-b2');
  try {
    const providerA = await cluster.startProvider('rm-b2-api-a', 'api-a');
    const providerB = await cluster.startProvider('rm-b2-api-b', 'api-b');
    const consumer = await cluster.startConsumer('rm-b2-consumer');
    await cluster.waitForProviders([providerA.channelEndpoint, providerB.channelEndpoint]);

    const baseline = await requestMany(consumer.httpUrl, uniqueMarker('rm-b2-before'), 20);
    ensure(baseline.some((reply) => reply.providerRid === 'api-a'), 'RM-B2 baseline did not reach api-a.');
    ensure(baseline.some((reply) => reply.providerRid === 'api-b'), 'RM-B2 baseline did not reach api-b.');

    const firstDuring = postJson<ProfileRes>(consumer.httpUrl, '/profile/request', {
      value: uniqueMarker('rm-b2-during')
    });
    const draining = cluster.drain(providerB);
    const during = await Promise.all([
      firstDuring,
      ...Array.from({ length: 19 }, (_, index) =>
        postJson<ProfileRes>(consumer.httpUrl, '/profile/request', {
          value: `rm-b2-during-${index}`
        }))
    ]);
    ensure(
      during.every((reply) => reply.providerRid === 'api-a' || reply.providerRid === 'api-b'),
      'RM-B2 in-flight traffic returned an unknown provider.'
    );
    const drainResult = await draining;
    ensure(
      drainResult.outcome === 0
        && drainResult.reason === 0,
      `RM-B2 provider retire failed: ${drainResult.outcome}/${drainResult.reason}.`
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
  return await Promise.all(Array.from({ length: count }, (_, index) =>
    postJson<ProfileRes>(consumerUrl, '/profile/request', { value: `${marker}-${index}` })));
}
