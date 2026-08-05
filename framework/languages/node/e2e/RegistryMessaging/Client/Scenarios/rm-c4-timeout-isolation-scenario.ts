// RM-C4: Timeout 뒤 late reply를 버린다 시나리오를 검증한다.
import type { ProfileRes, RequestFailureRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runRmC4(locationConsumerUrl: string, providerAUrl: string, providerBUrl: string): Promise<void> {
  const timeout = await postJson<RequestFailureRes>(locationConsumerUrl, '/profile/slow-request', { value: 'slow' });
  ensure(timeout.failed, 'RM-C4 expected the slow request to time out.');

  const immediate = await postJson<ProfileRes>(locationConsumerUrl, '/profile/request', { value: 'rm-c4-after-timeout' });
  ensure(immediate.value === 'profile:rm-c4-after-timeout', 'RM-C4 follow-up reply mismatch.');

  await new Promise((resolve) => setTimeout(resolve, 1200));
  const later = await postJson<ProfileRes>(locationConsumerUrl, '/profile/request', { value: 'rm-c4-later' });
  ensure(later.value === 'profile:rm-c4-later', 'RM-C4 later reply mismatch.');

  const afterTimeout = await firstEvidence(providerAUrl, providerBUrl, 'rm-c4-after-timeout');
  const laterEvidence = await firstEvidence(providerAUrl, providerBUrl, 'rm-c4-later');
  const lines = [...afterTimeout, ...laterEvidence];
  ensure(lines.some((line) => line.includes('rm-c4-after-timeout')) && lines.some((line) => line.includes('rm-c4-later')), 'RM-C4 follow-up evidence missing.');
  console.log('scenario RM-C4 passed');
}

async function firstEvidence(providerAUrl: string, providerBUrl: string, marker: string): Promise<string[]> {
  return await Promise.race([
    postJson<string[]>(providerAUrl, '/evidence/wait', { contains: marker }),
    postJson<string[]>(providerBUrl, '/evidence/wait', { contains: marker })
  ]);
}
