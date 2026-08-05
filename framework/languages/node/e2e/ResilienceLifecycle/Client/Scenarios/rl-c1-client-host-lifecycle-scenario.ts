// RL-C1: 다량 connection 뒤 정상 종료한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { profileReq } from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';

export async function runRlC1(options: ClientOptions): Promise<void> {
  for (let index = 0; index < 12; index += 1) {
    const reply = await postJson<ProfileRes>(
      options.consumerUrl,
      '/profile/request/new-client',
      profileReq(`rl-c1-${index}`)
    );
    ensure(reply.value === 'profile:fast', 'RL-C1 request failed before cleanup.');
  }

  const followUp = await postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request/new-client',
    profileReq('rl-c1-after-cleanup')
  );
  ensure(followUp.value === 'profile:fast', 'RL-C1 follow-up failed after client cleanup.');

  const lifecycleEvidence = await Promise.any([
    postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: 'marker=rl-c1-' }),
    postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-c1-' })
  ]);
  ensure(
    lifecycleEvidence.some((line) => line.includes('marker=rl-c1-')),
    'RL-C1 did not record expected lifecycle evidence.'
  );

  const followUpEvidence = await Promise.any([
    postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: 'marker=rl-c1-after-cleanup' }),
    postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-c1-after-cleanup' })
  ]);
  ensure(
    followUpEvidence.some((line) => line.includes('marker=rl-c1-after-cleanup')),
    'RL-C1 did not record expected cleanup follow-up evidence.'
  );

  console.log('scenario RL-C1 passed');
}
