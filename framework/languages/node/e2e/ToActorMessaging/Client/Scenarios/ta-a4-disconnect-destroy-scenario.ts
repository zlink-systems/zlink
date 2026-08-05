// TA-A4: Unbind 뒤에는 direct message가 계속되고 Actor 제거 뒤에는 실패한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import {
  type ActorEvidence, assertCall, assertFailure, bindActor, ensureActor,
  requireEvidence, requireNoEvidence
} from '../Support/actor-scenario-support';

export async function runTaA4(options: ClientOptions): Promise<void> {
  const actor = await ensureActor(options, 'ta-a4');
  const session = await bindActor(options, actor.actor);
  await session.close();
  await assertCall(options, 'TA-A4-disconnected-send', 'ta-a4', actor.actor, 'a4-send', 'sent', true);
  await assertCall(options, 'TA-A4-disconnected-request', 'ta-a4', actor.actor, 'a4-request', 'reply:a4-request', false);
  const evidence = await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`);
  requireEvidence(evidence, 'TA-A4-disconnected-send', 'send');
  requireEvidence(evidence, 'TA-A4-disconnected-request', 'request');
  await postJson(`${options.actorUrl}/actors/ta-a4/destroy`, {});
  await assertFailure(options, 'TA-A4-destroyed-request', 'ta-a4', 'NotFound', false, actor.actor);
  requireNoEvidence(await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`), 'TA-A4-destroyed-request');
  console.log('scenario TA-A4 passed');
}
