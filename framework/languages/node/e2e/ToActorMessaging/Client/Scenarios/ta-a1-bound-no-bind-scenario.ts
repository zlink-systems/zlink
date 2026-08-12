// TA-A1: Session binding과 독립적으로 Actor direct send·request를 보낸다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson } from '../../../http-client';
import {
  type ActorEvidence, assertBoundPush, assertCall, assertSameBinding, assertUnbound,
  bindActor, bindingSnapshot, ensureActor, requireEvidence
} from '../Support/actor-scenario-support';

export async function runTaA1(options: ClientOptions): Promise<void> {
  const boundActor = await ensureActor(options, 'ta-a1');
  const unboundActor = await ensureActor(options, 'ta-a2');
  assertUnbound(await bindingSnapshot(options, 'ta-a2'), 'TA-A1 unbound variant before direct calls');

  const session = await bindActor(options, boundActor.actor);
  try {
    const before = await bindingSnapshot(options, 'ta-a1');
    await assertBoundPush(session, 'TA-A1-push-before-no-bind', 'ta-a1', 'a1-push-before');
    await assertCall(options, 'TA-A1-bound-send', 'ta-a1', boundActor.actor, 'a1-bound-send', 'sent', true);
    await assertCall(
      options,
      'TA-A1-bound-request',
      'ta-a1',
      boundActor.actor,
      'a1-bound-request',
      'reply:a1-bound-request',
      false
    );
    await assertCall(options, 'TA-A1-unbound-send', 'ta-a2', unboundActor.actor, 'a1-unbound-send', 'sent', true);
    await assertCall(
      options,
      'TA-A1-unbound-request',
      'ta-a2',
      unboundActor.actor,
      'a1-unbound-request',
      'reply:a1-unbound-request',
      false
    );
    await assertBoundPush(session, 'TA-A1-push-after-no-bind', 'ta-a1', 'a1-push-after');
    assertSameBinding(before, await bindingSnapshot(options, 'ta-a1'), 'TA-A1');
    assertUnbound(await bindingSnapshot(options, 'ta-a2'), 'TA-A1 unbound variant after direct calls');
  } finally { await session.close(); }
  const evidence = await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`);
  requireEvidence(evidence, 'TA-A1-bound-send', 'send');
  requireEvidence(evidence, 'TA-A1-bound-request', 'request');
  requireEvidence(evidence, 'TA-A1-unbound-send', 'send');
  requireEvidence(evidence, 'TA-A1-unbound-request', 'request');
  requireEvidence(evidence, 'TA-A1-push-before-no-bind', 'push');
  requireEvidence(evidence, 'TA-A1-push-after-no-bind', 'push');
  console.log('scenario TA-A1 passed');
}
