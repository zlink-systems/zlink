// CH-E2E-04A: ClientServer weight에 따라 target을 선택한다 시나리오를 검증한다.
import { assert, postJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh04A(options: ClientOptions): Promise<void> {
  const marker = `ch-04a-${Date.now()}`;
  let high = 0;
  for (let index = 0; index < 800; index += 1) {
    const result = await postJson<{ succeeded: boolean; reply?: { role: string } }>(options.workflowCallerUrl, '/request', { channel: 'workflow.command', id: `${marker}-${index}` });
    assert.equal(result.succeeded, true);
    if (result.reply?.role === 'workflow-b') high += 1;
  }
  assert.ok(high >= 520 && high <= 680, `weight-300 selection count=${high}`);
}
