// RC-B1: 별도 등록 없이 기본 JSON을 사용한다 시나리오를 검증한다.
import type { EchoRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-B1 verifies JSON request and send round-trip evidence on the main host.
export async function runRcB1(serverUrl: string): Promise<void> {
  const reply = await postJson<EchoRes>(serverUrl, '/codec/json');
  ensure(reply.value === 'echo:rc-b1', 'RC-B1 reply value mismatch.');
  ensure(reply.contentType === 'application/json', 'RC-B1 expected application/json content type.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['codec=json', 'rc-b1-send', 'codec-reply|codec=json'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('codec-request|codec=json')), 'RC-B1 request evidence missing.');
  ensure(evidence.some((line) => line.includes('codec-command|codec=json')), 'RC-B1 command evidence missing.');
  console.log('scenario RC-B1 passed');
}
