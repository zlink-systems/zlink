// CH-E2E-01: 같은 RouteMesh에서 양방향 request를 보낸다 시나리오를 검증한다.
import { assert, postJson, waitForEvidence } from '../Support/scenario-support';
import type { ClientOptions } from '../Support/client-options';

export async function runCh01(options: ClientOptions): Promise<void> {
  const id = `ch-01-${Date.now()}`;
  const session = await postJson<{ reply?: { role: string }; succeeded: boolean }>(options.sessionUrl, '/request', { channel: 'game.play', id });
  const play = await postJson<{ reply?: { role: string }; succeeded: boolean }>(options.playUrl, '/request', { channel: 'game.session', id: `${id}-reverse` });
  assert.equal(session.succeeded, true);
  assert.equal(session.reply?.role, 'play');
  assert.equal(play.succeeded, true);
  assert.equal(play.reply?.role, 'session');
  await waitForEvidence(options.playUrl, `id=${id}`);
  await waitForEvidence(options.sessionUrl, `id=${id}-reverse`);
}
