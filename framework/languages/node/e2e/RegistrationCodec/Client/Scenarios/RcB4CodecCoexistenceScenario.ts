// RC-B4: 한 root에서 여러 codec을 함께 사용한다 시나리오를 검증한다.
import type { CodecScenarioRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-B4 verifies that JSON, Protobuf, and MessagePack coexist in one host-level codec registry.
export async function runRcB4(serverUrl: string): Promise<void> {
  const result = await postJson<CodecScenarioRes>(serverUrl, '/codec/roundtrip');
  ensure(result.json?.value === 'echo:rc-b1', 'RC-B4 JSON reply mismatch.');
  ensure(result.protobufValue?.includes('echo:rc-b2') === true, 'RC-B4 Protobuf reply mismatch.');
  ensure(result.protobufValue?.includes('content:application/x-protobuf') === true, 'RC-B4 Protobuf content type mismatch.');
  ensure(result.messagePackValue?.includes('echo:rc-b3') === true, 'RC-B4 MessagePack reply mismatch.');
  ensure(result.messagePackValue?.includes('content:application/x-msgpack') === true, 'RC-B4 MessagePack content type mismatch.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['codec-request|codec=json', 'codec-request|codec=protobuf', 'codec-request|codec=msgpack'],
    timeoutMilliseconds: 10_000
  });
  ensure(
    evidence.some((line) => line.includes('codec-request|codec=json') && line.includes('content=application/json')),
    'RC-B4 JSON evidence missing.'
  );
  ensure(
    evidence.some((line) => line.includes('codec-request|codec=protobuf') && line.includes('content=application/x-protobuf')),
    'RC-B4 Protobuf evidence missing.'
  );
  ensure(
    evidence.some((line) => line.includes('codec-request|codec=msgpack') && line.includes('content=application/x-msgpack')),
    'RC-B4 MessagePack evidence missing.'
  );
  console.log('scenario RC-B4 passed');
}
