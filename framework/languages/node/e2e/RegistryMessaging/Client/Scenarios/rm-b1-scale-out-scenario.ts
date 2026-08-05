// RM-B1: Traffic 처리 중 provider를 추가한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { DynamicClusterLauncher } from '../Support/dynamic-cluster-launcher';
import { getJson, postJson } from '../../../http-client';
import { countNewEvidence, ensure, uniqueMarker } from '../Support/scenario-assert';
import type { ProfileRes } from '../../Shared/messages';

export async function runRmB1(options: ClientOptions): Promise<void> {
  const cluster = await DynamicClusterLauncher.start(options, 'rm-b1');
  try {
    const providerA = await cluster.startProvider('api-a', 'api-a');
    const consumer = await cluster.startConsumer('rm-b1-consumer');
    await cluster.waitForSingleProvider('api-a', providerA.channelEndpoint);
    let beforeA = await getJson<string[]>(providerA.httpUrl, '/evidence');
    const markerBefore = uniqueMarker('rm-b1-before');
    for (let i = 0; i < 10; i += 1) {
      const reply = await postJson<ProfileRes>(consumer.httpUrl, '/profile/request', { value: `${markerBefore}-${i}` });
      ensure(reply.providerRid === 'api-a', 'RM-B1 before scale-out should reach api-a.');
    }
    const preScaleEvidence = await postJson<string[]>(providerA.httpUrl, '/evidence/wait', { contains: `${markerBefore}-9` });
    ensure(countNewEvidence(preScaleEvidence, beforeA, 'profile-request|rid=api-a', markerBefore) === 10, 'RM-B1 pre-scale evidence was not api-a only.');

    const providerB = await cluster.startProvider('api-b', 'api-b');
    await cluster.waitForProviders([providerA.channelEndpoint, providerB.channelEndpoint]);
    beforeA = await getJson<string[]>(providerA.httpUrl, '/evidence');
    const beforeB = await getJson<string[]>(providerB.httpUrl, '/evidence');
    const markerAfter = uniqueMarker('rm-b1-after');
    const values = Array.from({ length: 40 }, (_, index) => `${markerAfter}-${index}`);
    const replies: Array<{ requestValue: string; reply: ProfileRes }> = [];
    for (const value of values) {
      replies.push({
        requestValue: value,
        reply: await postJson<ProfileRes>(consumer.httpUrl, '/profile/request', { value })
      });
    }
    const apiAValues = replies.filter((entry) => entry.reply.providerRid === 'api-a').map((entry) => entry.requestValue);
    const apiBValues = replies.filter((entry) => entry.reply.providerRid === 'api-b').map((entry) => entry.requestValue);
    ensure(apiAValues.length > 0 && apiBValues.length > 0, 'RM-B1 expected both providers after scale-out.');
    const afterA = await postJson<string[]>(providerA.httpUrl, '/evidence/wait', { contains: apiAValues[apiAValues.length - 1] });
    const afterB = await postJson<string[]>(providerB.httpUrl, '/evidence/wait', { contains: apiBValues[apiBValues.length - 1] });
    const a = countNewEvidence(afterA, beforeA, 'profile-request|rid=api-a', markerAfter);
    const b = countNewEvidence(afterB, beforeB, 'profile-request|rid=api-b', markerAfter);
    ensure(a === apiAValues.length && b === apiBValues.length && a + b === values.length, 'RM-B1 evidence did not match scale-out replies.');
    console.log('scenario RM-B1 passed');
  } finally {
    await cluster.close();
  }
}
