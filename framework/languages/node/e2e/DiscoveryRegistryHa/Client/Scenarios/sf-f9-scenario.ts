// SF-F9: Old lifecycle cleanup이 replacement service roles를 제거하지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import type { ProfileRes } from '../../Shared/messages';
import { ensure } from '../Support/scenario-assert';

export async function runSFF9(options: ClientOptions): Promise<void> {
  const marker = `sf-f9-${Date.now().toString(36)}`;
  const replies = await Promise.all(Array.from({ length: 20 }, (_, index) => {
    const value = `${marker}-${index}`;
    return postJson<ProfileRes>(options.consumerUrl, '/profile/request', {
      value,
      marker: 'SF-F9'
    });
  }));
  ensure(replies.every((reply) => reply.value.startsWith(`profile:${marker}-`)), 'SF-F9 reply payload mismatch.');
  ensure(replies.every((reply) => reply.providerRid === 'api-a'), 'SF-F9 selected the old lifecycle or another provider.');
  const deadline = Date.now() + 3000;
  let evidence: readonly string[] = [];
  while (Date.now() < deadline) {
    evidence = await getJson<readonly string[]>(options.providerAUrl, '/evidence');
    const matching = evidence.filter(
      (line) => line.includes(`value=${marker}-`) && line.includes('marker=SF-F9')
    );
    if (matching.length === replies.length
      && new Set(matching.map((line) => line.match(/value=([^|]+)/)?.[1])).size === replies.length) break;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  const matching = evidence.filter(
    (line) => line.includes(`value=${marker}-`) && line.includes('marker=SF-F9')
  );
  const values = new Set(matching.map((line) => line.match(/value=([^|]+)/)?.[1]));
  ensure(
    matching.length === replies.length && values.size === replies.length
      && replies.every((_, index) => values.has(`${marker}-${index}`)),
    'SF-F9 replacement handler evidence is incomplete.'
  );
  console.log('scenario SF-F9 passed');
}
