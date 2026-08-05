// CH-E2E-07C: Known target에 연결할 수 없으면 Unavailable이다 시나리오를 검증한다.
import { assert, postJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh07C(options: ClientOptions): Promise<void> {
  const result = await postJson<{ succeeded: boolean; error?: string }>(options.workflowCallerUrl, '/request', { channel: 'workflow.command', id: `ch-07c-${Date.now()}` });
  assert.equal(result.succeeded, false);
  assert.equal(result.error, 'Unavailable');
}
