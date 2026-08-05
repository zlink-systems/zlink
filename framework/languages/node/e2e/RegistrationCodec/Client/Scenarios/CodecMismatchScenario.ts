// RC-B5: 수신 codec이 없으면 `ProtocolError`로 끝난다 시나리오를 검증한다.
import type { EchoRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-B5 verifies codec mismatch rejection against the JSON-only peer and JSON recovery afterward.
export async function runRcB5(codecRequesterUrl: string, jsonOnlyUrl: string): Promise<void> {
  const mismatch = await postJson<{ readonly rejected: boolean; readonly failureType?: string }>(codecRequesterUrl, '/codec/mismatch');
  ensure(mismatch.rejected, 'RC-B5 expected Protobuf request to JSON-only peer to be rejected.');
  const recovery = await postJson<EchoRes>(jsonOnlyUrl, '/codec/json-recovery');
  ensure(recovery.value === 'echo:rc-b5-json', 'RC-B5 JSON recovery reply mismatch.');
  ensure(recovery.contentType === 'application/json', 'RC-B5 JSON recovery expected JSON content type.');
  const evidence = await postJson<readonly string[]>(jsonOnlyUrl, '/evidence/wait', {
    containsAll: ['codec-mismatch-json|value=rc-b5-json', 'codec-mismatch-json-recovery|status=ok'],
    timeoutMilliseconds: 10_000
  });
  ensure(evidence.some((line) => line.includes('codec-mismatch-json|value=rc-b5-json')), 'RC-B5 JSON recovery evidence missing.');
  console.log('scenario RC-B5 passed');
}
