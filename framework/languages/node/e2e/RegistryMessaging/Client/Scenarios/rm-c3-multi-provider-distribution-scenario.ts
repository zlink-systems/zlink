// RM-C3: 같은 weight의 provider에 request를 분산한다 시나리오를 검증한다.
import type { ProfileRes, ProfileReq } from '../../Shared/messages';
import { getJson, postJson } from '../../../http-client';
import { countNewEvidence, ensure, uniqueMarker } from '../Support/scenario-assert';

export async function runRmC3(directConsumerUrl: string, providerAUrl: string, providerBUrl: string): Promise<void> {
  const beforeA = await getJson<string[]>(providerAUrl, '/evidence');
  const beforeB = await getJson<string[]>(providerBUrl, '/evidence');
  const marker = uniqueMarker('rm-c3');
  const requests: ProfileReq[] = Array.from({ length: 60 }, (_, index) => ({ value: `${marker}-${index}` }));

  const replies = await postJson<ProfileRes[]>(directConsumerUrl, '/profile/batch-request', requests);
  ensure(replies.length === requests.length, 'RM-C3 reply count mismatch.');
  for (let i = 0; i < requests.length; i += 1) {
    ensure(replies[i].value === `profile:${marker}-${i}`, 'RM-C3 reply value mismatch.');
    ensure(replies[i].providerRid === 'api-a' || replies[i].providerRid === 'api-b', 'RM-C3 reply provider mismatch.');
  }

  const afterA = await getJson<string[]>(providerAUrl, '/evidence');
  const afterB = await getJson<string[]>(providerBUrl, '/evidence');
  const a = countNewEvidence(afterA, beforeA, 'profile-request|rid=api-a', marker);
  const b = countNewEvidence(afterB, beforeB, 'profile-request|rid=api-b', marker);
  ensure(a > 0 && b > 0 && a + b === requests.length, 'RM-C3 expected both providers to handle the request set.');
  console.log('scenario RM-C3 passed');
}
