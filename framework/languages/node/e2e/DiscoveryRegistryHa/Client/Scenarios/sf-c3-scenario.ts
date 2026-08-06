// SF-C3: 이전 owner lifecycle이 replacement를 바꾸지 못한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import type { ProfileRes } from '../../Shared/messages';
import { ensure } from '../Support/scenario-assert';

export async function runSFC3(options: ClientOptions): Promise<void> {
  const marker = `sf-c3-${Date.now().toString(36)}`;
  const replies = await Promise.all(Array.from({ length: 20 }, (_, index) => {
    const value = `${marker}-${index}`;
    return postJson<ProfileRes>(options.consumerUrl, '/profile/request', {
      value,
      marker: 'SF-C3'
    });
  }));
  ensure(replies.every((reply) => reply.value.startsWith('profile:' + marker + '-')), 'SF-C3 reply payload mismatch.');
  ensure(replies.every((reply) => reply.providerRid === 'api-a'), 'SF-C3 selected the old lifecycle or another provider.');
  const evidence = await getJson<readonly string[]>(options.providerAUrl, '/evidence');
  ensure(
    replies.every((_, index) => evidence.some((line) => line.includes(`value=${marker}-${index}`))),
    'SF-C3 replacement handler evidence is incomplete.'
  );
  console.log('scenario SF-C3 passed');
}
