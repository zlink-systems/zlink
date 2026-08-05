// CH-E2E-09: Port 0과 advertised host로 remote connection을 만든다 시나리오를 검증한다.
import { assert, getJson } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh09(options: ClientOptions): Promise<void> {
  const route = await getJson<{ isReady: boolean; peers: readonly { rid: string }[] }>(options.sessionUrl, '/status/route');
  const workflow = await getJson<{ isReady: boolean; targets: readonly { rid: string }[] }>(options.workflowCallerUrl, '/status/workflow');
  assert.equal(route.isReady, true);
  assert.equal(workflow.isReady, true);
  assert.ok(route.peers.length >= 1);
  assert.ok(workflow.targets.length >= 1);
}
