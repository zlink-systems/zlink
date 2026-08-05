// RC-B2: Root에 등록한 Protobuf extension을 사용한다 시나리오를 검증한다.
import type { CodecScenarioRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-B2 verifies Protobuf content-type selection on the main host's global codec registry.
export async function runRcB2(serverUrl: string): Promise<void> {
  const reply = await postJson<CodecScenarioRes>(serverUrl, '/codec/protobuf');
  ensure(reply.value === 'echo:rc-b2', 'RC-B2 reply value mismatch.');
  ensure(reply.contentType === 'application/x-protobuf', 'RC-B2 expected Protobuf content type.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['codec=protobuf', 'rc-b2-send', 'codec-reply|codec=protobuf'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('codec-request|codec=protobuf')), 'RC-B2 request evidence missing.');
  ensure(evidence.some((line) => line.includes('codec-command|codec=protobuf')), 'RC-B2 command evidence missing.');
  console.log('scenario RC-B2 passed');
}
