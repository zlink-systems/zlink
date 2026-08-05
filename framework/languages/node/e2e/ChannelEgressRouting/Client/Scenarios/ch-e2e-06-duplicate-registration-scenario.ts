// CH-E2E-06: 같은 ChannelName을 여러 송신 경로에 등록하면 시작하지 못한다 시나리오를 검증한다.
import { assert, getJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh06(options: ClientOptions): Promise<void> {
  await assert.rejects(
    () => getJson(options.invalidUrl, '/health'),
    /fetch failed|ECONNREFUSED|socket hang up|other side closed/i
  );
}
