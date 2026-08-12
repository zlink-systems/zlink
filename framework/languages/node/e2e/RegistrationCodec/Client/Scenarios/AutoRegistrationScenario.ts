// RC-A1: 언어별 public scan 방식으로 handler를 등록한다 시나리오를 검증한다.
import type { EchoRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-A1 verifies that provider discovery registers request and send handlers without manual registration.
export async function runRcA1(serverUrl: string): Promise<void> {
  const reply = await postJson<EchoRes>(serverUrl, '/registration/auto');
  ensure(reply.value === 'echo:rc-a1', 'RC-A1 reply value mismatch.');
  ensure(reply.contentType === 'application/json', 'RC-A1 expected JSON content type.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['variant=auto', 'rc-a1-send'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('echo-request|variant=auto')), 'RC-A1 request evidence missing.');
  ensure(evidence.some((line) => line.includes('echo-command|variant=auto')), 'RC-A1 send evidence missing.');

  const attributedReply = await postJson<EchoRes>(serverUrl, '/registration/attribute');
  ensure(attributedReply.value === 'echo:rc-a2', 'RC-A1 attributed reply value mismatch.');
  const attributedEvidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['variant=attr', 'rc-a2-send'],
    timeoutMilliseconds: 10_000
  });
  ensure(
    attributedEvidence.some((line) => line.includes('echo-request|variant=attr')),
    'RC-A1 attributed request evidence missing.'
  );
  ensure(
    attributedEvidence.some((line) => line.includes('echo-command|variant=attr')),
    'RC-A1 attributed send evidence missing.'
  );
  console.log('scenario RC-A1 passed');
}
