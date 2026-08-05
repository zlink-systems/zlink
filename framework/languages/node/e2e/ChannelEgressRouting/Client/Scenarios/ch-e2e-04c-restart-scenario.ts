// CH-E2E-04C: Server 재시작 뒤 신규 request를 처리한다 시나리오를 검증한다.
import { assert, postJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh04C(options: ClientOptions): Promise<void> {
  const id = `ch-04c-${Date.now()}`;
  const result = await postJson<{ succeeded: boolean; reply?: { role: string; lifecycle?: string } }>(options.workflowCallerUrl, '/request', { channel: 'workflow.command', id });
  assert.equal(result.succeeded, true);
  assert.equal(result.reply?.role, options.expectedWorkflowRid);
  assert.equal(result.reply?.lifecycle, options.expectedWorkflowLifecycle);
}
