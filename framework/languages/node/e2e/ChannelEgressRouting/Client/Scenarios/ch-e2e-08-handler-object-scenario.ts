// CH-E2E-08: ClientServer handler가 Spot과 Actor를 연속 호출한다 시나리오를 검증한다.
import { assert, postJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh08(options: ClientOptions): Promise<void> {
  const result = await postJson<{ succeeded: boolean; reply?: { role: string; downstream: readonly string[] } }>(options.sessionUrl, '/request', { channel: 'game.play', id: `ch-08-${Date.now()}`, mode: 'cascade' });
  assert.equal(result.succeeded, true);
  assert.equal(result.reply?.role, 'play');
  assert.equal(result.reply?.downstream.length, 2);
}
