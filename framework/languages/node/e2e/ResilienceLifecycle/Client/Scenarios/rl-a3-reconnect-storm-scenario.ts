// RL-A3: 많은 clients가 server restart 뒤 reconnect한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { profileReq } from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';

export async function runRlA3(options: ClientOptions): Promise<void> {
  for (let i = 0; i < 24; i += 1) {
    const marker = `rl-a3-${i}`;
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request/new-client', profileReq(marker));
    ensure(
      reply.value === 'profile:fast' && (reply.providerRid === 'api-a' || reply.providerRid === 'api-b'),
      'RL-A3 storm request returned an unexpected reply.'
    );
  }

  const evidence = await Promise.race([
    postJson<string[]>(options.providerAUrl, '/evidence/wait', { contains: 'marker=rl-a3-', timeoutMilliseconds: 15000 }),
    postJson<string[]>(options.providerBUrl, '/evidence/wait', { contains: 'marker=rl-a3-', timeoutMilliseconds: 15000 })
  ]);
  ensure(
    evidence.some((line) => line.includes('marker=rl-a3-')),
    'RL-A3 did not record expected provider evidence.'
  );

  console.log('scenario RL-A3 passed');
}
