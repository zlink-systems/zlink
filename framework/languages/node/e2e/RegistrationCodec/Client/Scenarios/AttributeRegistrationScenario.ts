// RC-A2: 언어별 annotation·attribute handler를 scan한다 시나리오를 검증한다.
import type { EchoRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-A2 verifies Node decorator registration as the language mapping for attribute-style handlers.
export async function runRcA2(serverUrl: string): Promise<void> {
  const reply = await postJson<EchoRes>(serverUrl, '/registration/attribute');
  ensure(reply.value === 'echo:rc-a2', 'RC-A2 reply value mismatch.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['variant=attr', 'rc-a2-send'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('echo-request|variant=attr')), 'RC-A2 request evidence missing.');
  ensure(evidence.some((line) => line.includes('echo-command|variant=attr')), 'RC-A2 send evidence missing.');
  console.log('scenario RC-A2 passed');
}
