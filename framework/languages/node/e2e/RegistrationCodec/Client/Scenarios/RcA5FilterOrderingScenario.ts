// RC-A5: Filter 순서와 short-circuit 결과 시나리오를 검증한다.
import type { EchoRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-A5 verifies that registered handler filters run before and after the handler in registration order.
export async function runRcA5(serverUrl: string): Promise<void> {
  const reply = await postJson<EchoRes>(serverUrl, '/registration/filter-order');
  ensure(reply.value === 'echo:rc-a5', 'RC-A5 reply value mismatch.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: [
      'filter|name=first|phase=before|packet=EchoManualReq',
      'filter|name=second|phase=before|packet=EchoManualReq',
      'filter|name=second|phase=after|packet=EchoManualReq',
      'filter|name=first|phase=after|packet=EchoManualReq',
      'filter-reply|value=echo:rc-a5'
    ],
    timeoutMilliseconds: 10_000
  });
  const order = evidence
    .filter((line) => line.includes('filter|') && line.includes('packet=EchoManualReq'))
    .slice(-4)
    .map((line) => {
      const name = line.includes('name=first') ? 'first' : 'second';
      const phase = line.includes('phase=before') ? 'before' : 'after';
      return `${name}:${phase}`;
    });
  ensure(
    order.join(',') === 'first:before,second:before,second:after,first:after',
    `RC-A5 filter order mismatch: ${order.join(',')}`
  );
  console.log('scenario RC-A5 passed');
}
