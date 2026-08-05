// RC-A3: Handler를 명시적으로 등록한다 시나리오를 검증한다.
import type { EchoRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-A3 verifies explicit handler registration as the control case for request and send handling.
export async function runRcA3(serverUrl: string): Promise<void> {
  const reply = await postJson<EchoRes>(serverUrl, '/registration/manual');
  ensure(reply.value === 'echo:rc-a3', 'RC-A3 reply value mismatch.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['variant=manual', 'rc-a3-send'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('echo-request|variant=manual')), 'RC-A3 request evidence missing.');
  ensure(evidence.some((line) => line.includes('echo-command|variant=manual')), 'RC-A3 send evidence missing.');
  console.log('scenario RC-A3 passed');
}
