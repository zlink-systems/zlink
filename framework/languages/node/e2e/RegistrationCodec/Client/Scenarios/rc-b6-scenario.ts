// RC-B6: 다섯 언어가 JSON application 값을 같게 복원한다 시나리오를 검증한다.
import type { CodecScenarioRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export const scenarioId = 'RC-B6' as const;

export async function runRCB6(serverUrl: string): Promise<void> {
  const result = await postJson<CodecScenarioRes>(serverUrl, '/codec/roundtrip');
  ensure(result.json?.value === 'echo:rc-b1', 'RC-B6 JSON value mismatch.');
  ensure(result.json?.contentType === 'application/json', 'RC-B6 JSON content type mismatch.');
  ensure(result.protobufValue?.includes('echo:rc-b2') === true, 'RC-B6 Protobuf value mismatch.');
  ensure(result.messagePackValue?.includes('echo:rc-b3') === true, 'RC-B6 MessagePack value mismatch.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['codec-request|codec=json', 'codec-request|codec=protobuf', 'codec-request|codec=msgpack'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('codec-request|codec=json')), 'RC-B6 JSON evidence missing.');
  ensure(evidence.some((line) => line.includes('codec-request|codec=protobuf')), 'RC-B6 Protobuf evidence missing.');
  ensure(evidence.some((line) => line.includes('codec-request|codec=msgpack')), 'RC-B6 MessagePack evidence missing.');
  console.log('scenario RC-B6 passed');
}
