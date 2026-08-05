// CH-E2E-11: ChannelName만으로 다른 MeshNode의 Server를 호출한다 시나리오를 검증한다.
import { assert, postJson, waitForEvidence } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh11(options: ClientOptions): Promise<void> {
  const id = `ch-11-${Date.now()}`;
  const request = await postJson<{ succeeded: boolean; reply?: { role: string } }>(options.sessionUrl, '/request', { channel: 'game.api', id });
  assert.equal(request.succeeded, true);
  assert.ok(request.reply?.role === 'api' || request.reply?.role === 'api-a' || request.reply?.role === 'api-b');
  const send = await postJson<{ succeeded: boolean }>(options.sessionUrl, '/send', { channel: 'game.api', id: `${id}-send` });
  assert.equal(send.succeeded, true);
  await Promise.race([waitForEvidence(options.apiAUrl, `id=${id}-send`), waitForEvidence(options.apiBUrl, `id=${id}-send`)]);
}
