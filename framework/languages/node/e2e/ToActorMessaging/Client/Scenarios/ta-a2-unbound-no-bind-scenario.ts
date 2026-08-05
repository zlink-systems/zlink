// TA-A2: Bind되지 않은 Actor에게 direct send·request를 보낸다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson } from '../../../http-client';
import { type ActorEvidence, assertCall, assertUnbound, bindingSnapshot, ensureActor, requireEvidence } from '../Support/actor-scenario-support';

export async function runTaA2(options: ClientOptions): Promise<void> {
  const actor = await ensureActor(options, 'ta-a2');
  assertUnbound(await bindingSnapshot(options, 'ta-a2'), 'TA-A2 before no-bind calls');
  await assertCall(options, 'TA-A2-unbound-send', 'ta-a2', actor.actor, 'a2-send', 'sent', true);
  await assertCall(options, 'TA-A2-unbound-request', 'ta-a2', actor.actor, 'a2-request', 'reply:a2-request', false);
  assertUnbound(await bindingSnapshot(options, 'ta-a2'), 'TA-A2 after no-bind calls');
  const evidence = await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`);
  requireEvidence(evidence, 'TA-A2-unbound-send', 'send');
  requireEvidence(evidence, 'TA-A2-unbound-request', 'request');
  console.log('scenario TA-A2 passed');
}
