// RC-A4: Dispatch마다 dependency scope를 분리한다 시나리오를 검증한다.
import type { EchoRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

// RC-A4 verifies per-dispatch scoped dependency separation and singleton stability through the public dispatch path.
export async function runRcA4(serverUrl: string): Promise<void> {
  const replies = await postJson<readonly EchoRes[]>(serverUrl, '/registration/di-filter-order');
  ensure(replies.length === 2, 'RC-A4 expected two replies.');
  ensure(replies[0]?.value === 'echo:rc-a4-1', 'RC-A4 first reply mismatch.');
  ensure(replies[1]?.value === 'echo:rc-a4-2', 'RC-A4 second reply mismatch.');
  const evidence = await postJson<readonly string[]>(serverUrl, '/evidence/wait', {
    containsAll: ['di|value=rc-a4-1', 'di|value=rc-a4-2'],
    timeoutMilliseconds: 10_000
  });
  const di = evidence.filter((line) => line.includes('di|'));
  const singletonIds = new Set(di.map((line) => extractValue(line, 'singleton')));
  const scopedIds = new Set(di.map((line) => extractValue(line, 'scoped')));
  ensure(di.length >= 2, 'RC-A4 DI evidence missing.');
  ensure(singletonIds.size === 1, 'RC-A4 expected stable singleton dependency.');
  ensure(scopedIds.size >= 2, 'RC-A4 expected per-dispatch scoped dependencies.');
  console.log('scenario RC-A4 passed');
}

function extractValue(line: string, key: string): string {
  const marker = `${key}=`;
  const start = line.indexOf(marker);
  if (start < 0) {
    return '';
  }
  const valueStart = start + marker.length;
  const end = line.indexOf('|', valueStart);
  return end < 0 ? line.slice(valueStart) : line.slice(valueStart, end);
}
