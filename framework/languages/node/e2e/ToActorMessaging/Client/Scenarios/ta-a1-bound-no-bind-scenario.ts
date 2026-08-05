// TA-A1: Bind된 Actor에게 direct send·request를 보낸다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson } from '../../../http-client';
import {
  type ActorEvidence, assertBoundPush, assertCall, assertSameBinding,
  bindActor, bindingSnapshot, ensureActor, requireEvidence
} from '../Support/actor-scenario-support';

export async function runTaA1(options: ClientOptions): Promise<void> {
  const actor = await ensureActor(options, 'ta-a1');
  const session = await bindActor(options, actor.actor);
  try {
    const before = await bindingSnapshot(options, 'ta-a1');
    await assertBoundPush(session, 'TA-A1-push-before-no-bind', 'ta-a1', 'a1-push-before');
    await assertCall(options, 'TA-A1-send', 'ta-a1', actor.actor, 'a1-send', 'sent', true);
    await assertCall(options, 'TA-A1-request', 'ta-a1', actor.actor, 'a1-request', 'reply:a1-request', false);
    await assertBoundPush(session, 'TA-A1-push-after-no-bind', 'ta-a1', 'a1-push-after');
    assertSameBinding(before, await bindingSnapshot(options, 'ta-a1'), 'TA-A1');
  } finally { await session.close(); }
  const evidence = await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`);
  requireEvidence(evidence, 'TA-A1-send', 'send');
  requireEvidence(evidence, 'TA-A1-request', 'request');
  requireEvidence(evidence, 'TA-A1-push-before-no-bind', 'push');
  requireEvidence(evidence, 'TA-A1-push-after-no-bind', 'push');
  console.log('scenario TA-A1 passed');
}
