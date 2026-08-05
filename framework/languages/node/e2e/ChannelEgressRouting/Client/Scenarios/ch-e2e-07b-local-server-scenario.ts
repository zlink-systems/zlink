// CH-E2E-07B: Local Server role만 있어도 remote member를 호출한다 시나리오를 검증한다.
import { assert, postJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh07B(options: ClientOptions): Promise<void> {
  let selected = 0;
  for (let index = 0; index < 20; index += 1) {
    const result = await postJson<{ succeeded: boolean; reply?: { role: string } }>(options.apiAUrl, '/request', { channel: 'game.api', id: `ch-07b-${Date.now()}-${index}` });
    assert.equal(result.succeeded, true);
    if (result.reply?.role === 'api-b') selected += 1;
  }
  assert.ok(selected > 0, 'API server selected no remote member.');
}
