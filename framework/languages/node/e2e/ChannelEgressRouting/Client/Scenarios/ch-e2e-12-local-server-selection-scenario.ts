// CH-E2E-12: 같은 process의 Client와 Server도 일반 후보로 선택한다 시나리오를 검증한다.
import { assert, postJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh12(options: ClientOptions): Promise<void> {
  let local = 0;
  let remote = 0;
  for (let index = 0; index < 400; index += 1) {
    const result = await postJson<{ succeeded: boolean; reply?: { role: string } }>(options.workflowAUrl, '/request', { channel: 'workflow.command', id: `ch-12-${Date.now()}-${index}` });
    assert.equal(result.succeeded, true);
    if (result.reply?.role === 'workflow-a') local += 1;
    if (result.reply?.role === 'workflow-b') remote += 1;
  }
  assert.ok(local > 0 && remote > 0, `local=${local} remote=${remote}`);
}
