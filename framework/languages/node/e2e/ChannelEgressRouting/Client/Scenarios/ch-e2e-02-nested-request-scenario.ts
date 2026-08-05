// CH-E2E-02: Handler가 다른 topology의 Channel을 호출한다 시나리오를 검증한다.
import { assert, postJson, waitForEvidence } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh02(options: ClientOptions): Promise<void> {
  const id = `ch-02-${Date.now()}`;
  const result = await postJson<{ succeeded: boolean; reply?: { role: string; downstream: readonly string[] } }>(options.sessionUrl, '/request', {
    channel: 'game.play', id, mode: 'cascade'
  });
  assert.equal(result.succeeded, true);
  assert.equal(result.reply?.role, 'play');
  assert.equal(result.reply?.downstream.length, 2);
  await waitForEvidence(options.auditUrl, `id=${id}-audit`);
  await Promise.race([waitForEvidence(options.workflowAUrl, `id=${id}-workflow`), waitForEvidence(options.workflowBUrl, `id=${id}-workflow`)]);
}
