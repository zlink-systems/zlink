// RL-B6: 한 provider의 gray failure를 다른 replies와 격리한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { profileReq } from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';

export async function runRlB6(options: ClientOptions): Promise<void> {
  await postJson(options.providerBUrl, '/admin/fault/gray');

  let failures = 0;
  let healthySuccesses = 0;
  for (let i = 0; i < 60 && (failures === 0 || healthySuccesses === 0); i += 1) {
    try {
      const reply = await postJson<ProfileRes>(
        options.consumerUrl,
        '/profile/request/no-retry',
        profileReq(`rl-b6-${i}`)
      );
      if (reply.providerRid === 'api-a') {
        healthySuccesses += 1;
      }
    } catch {
      failures += 1;
    }
  }

  ensure(healthySuccesses > 0 && failures > 0, 'RL-B6 expected both healthy successes and gray failures.');

  await postJson(options.providerBUrl, '/admin/fault/none');
  const followUp = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request',
    { value: 'fast', marker: 'rl-b6-after' }
  );
  ensure(followUp.value === 'profile:fast', 'RL-B6 follow-up request failed after clearing fault.');

  const grayEvidence = await Promise.any([
    postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: 'marker=rl-b6-' }),
    postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-b6-' })
  ]);
  ensure(
    grayEvidence.some((line) => line.includes('marker=rl-b6-')),
    'RL-B6 did not record expected gray-failure evidence.'
  );

  const afterEvidence = await Promise.any([
    postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: 'marker=rl-b6-after' }),
    postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-b6-after' })
  ]);
  ensure(
    afterEvidence.some((line) => line.includes('marker=rl-b6-after')),
    'RL-B6 did not record expected recovery evidence.'
  );

  console.log('scenario RL-B6 passed');
}
