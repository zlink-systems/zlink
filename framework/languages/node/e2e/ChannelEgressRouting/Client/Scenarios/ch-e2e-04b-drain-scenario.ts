// CH-E2E-04B: Draining server는 신규 request에서 제외한다 시나리오를 검증한다.
import { assert, getJson, postJson, waitFor, waitForEvidence } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh04B(options: ClientOptions): Promise<void> {
  const id = `ch-04b-${Date.now()}`;
  const held = postJson<{ succeeded: boolean; reply?: { role: string } }>(options.workflowCallerUrl, '/request', { channel: 'workflow.command', id, mode: 'hold' });
  const winner = await Promise.race([
    waitForEvidence(options.workflowAUrl, `id=${id}`).then((evidence) => ({ url: options.workflowAUrl, role: 'workflow-a', evidence })),
    waitForEvidence(options.workflowBUrl, `id=${id}`).then((evidence) => ({ url: options.workflowBUrl, role: 'workflow-b', evidence }))
  ]);
  await postJson(winner.url, '/control/release', {});
  const first = await held;
  assert.equal(first.succeeded, true);
  assert.equal(first.reply?.role, winner.role);
  await postJson(winner.url, '/drain', {});
  await waitFor(
    () => getJson<{ readonly isReady: boolean; readonly readyTargetCount: number; readonly targets: readonly { readonly rid: string; readonly state: string }[] }>(options.workflowCallerUrl, '/status/workflow'),
    (status) => status.isReady
      && status.readyTargetCount === 1
      && status.targets.some((target) => target.state === '1'),
    'remaining workflow target after drain'
  );
  for (let index = 0; index < 50; index += 1) {
    const next = await postJson<{ succeeded: boolean; reply?: { role: string } }>(options.workflowCallerUrl, '/request', { channel: 'workflow.command', id: `${id}-new-${index}` });
    assert.equal(next.succeeded, true);
    assert.notEqual(next.reply?.role, winner.role);
  }
}
