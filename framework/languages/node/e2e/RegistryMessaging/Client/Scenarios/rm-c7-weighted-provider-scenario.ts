// RM-C7: Weight에 따라 provider 선택 비율을 정한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { DynamicClusterLauncher } from '../Support/dynamic-cluster-launcher';
import { getJson, postJson } from '../../../http-client';
import { countNewEvidence, ensure, uniqueMarker } from '../Support/scenario-assert';
import type { ProfileRes } from '../../Shared/messages';

export async function runRmC7(options: ClientOptions): Promise<void> {
  const cluster = await DynamicClusterLauncher.start(options, 'rm-c7');
  try {
    const providerA = await cluster.startProvider('api-a-weighted', 'api-a', 75);
    const providerB = await cluster.startProvider('api-b-weighted', 'api-b', 25);
    const beforeA = await getJson<string[]>(providerA.httpUrl, '/evidence');
    const beforeB = await getJson<string[]>(providerB.httpUrl, '/evidence');
    const marker = uniqueMarker('rm-c7');
    const values = Array.from({ length: 240 }, (_, index) => `${marker}-${index}`);
    const replies: Array<{ requestValue: string; reply: ProfileRes }> = [];

    for (const value of values) {
      replies.push({
        requestValue: value,
        reply: await postJson<ProfileRes>(providerA.httpUrl, '/profile/request', { value })
      });
    }

    ensure(replies.length === values.length, 'RM-C7 reply count mismatch.');
    ensure(
      replies.every((entry) => entry.reply.providerRid === 'api-a' || entry.reply.providerRid === 'api-b'),
      'RM-C7 reply provider mismatch.'
    );

    const apiAValues = replies.filter((entry) => entry.reply.providerRid === 'api-a').map((entry) => entry.requestValue);
    const apiBValues = replies.filter((entry) => entry.reply.providerRid === 'api-b').map((entry) => entry.requestValue);
    ensure(apiAValues.length > 0 && apiBValues.length > 0, 'RM-C7 expected both weighted providers.');

    const afterA = await postJson<string[]>(providerA.httpUrl, '/evidence/wait', {
      contains: apiAValues[apiAValues.length - 1],
      timeoutMilliseconds: 20000
    });
    const afterB = await postJson<string[]>(providerB.httpUrl, '/evidence/wait', {
      contains: apiBValues[apiBValues.length - 1],
      timeoutMilliseconds: 20000
    });
    const a = countNewEvidence(afterA, beforeA, 'profile-request|rid=api-a', marker);
    const b = countNewEvidence(afterB, beforeB, 'profile-request|rid=api-b', marker);
    ensure(
      a === apiAValues.length &&
        b === apiBValues.length &&
        a + b === values.length &&
        a > b * 2,
      'RM-C7 weighted provider counts did not favor api-a.'
    );
    console.log('scenario RM-C7 passed');
  } finally {
    await cluster.close();
  }
}
