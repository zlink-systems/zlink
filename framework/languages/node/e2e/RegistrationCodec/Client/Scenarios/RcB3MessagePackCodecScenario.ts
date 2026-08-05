// RC-B3: Root에 등록한 MessagePack extension을 사용한다 시나리오를 검증한다.
import type { CodecScenarioRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-B3 verifies MessagePack content-type selection on the main host's global codec registry.
export async function runRcB3(serverUrl: string): Promise<void> {
  const reply = await postJson<CodecScenarioRes>(serverUrl, '/codec/msgpack');
  ensure(reply.value === 'echo:rc-b3', 'RC-B3 reply value mismatch.');
  ensure(reply.contentType === 'application/x-msgpack', 'RC-B3 expected MessagePack content type.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['codec=msgpack', 'rc-b3-send', 'codec-reply|codec=msgpack'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('codec-request|codec=msgpack')), 'RC-B3 request evidence missing.');
  ensure(evidence.some((line) => line.includes('codec-command|codec=msgpack')), 'RC-B3 command evidence missing.');
  console.log('scenario RC-B3 passed');
}
