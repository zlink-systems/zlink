// TA-A3: Direct message 뒤에 Session을 bind한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson } from '../../../http-client';
import {
  type ActorEvidence, assertBound, assertBoundPush, assertCall, assertUnbound,
  bindActor, bindingSnapshot, ensureActor, requireEvidence
} from '../Support/actor-scenario-support';

export async function runTaA3(options: ClientOptions): Promise<void> {
  const actor = await ensureActor(options, 'ta-a3');
  assertUnbound(await bindingSnapshot(options, 'ta-a3'), 'TA-A3 before no-bind calls');
  await assertCall(options, 'TA-A3-before-bind-send', 'ta-a3', actor.actor, 'a3-before-send', 'sent', true);
  await assertCall(options, 'TA-A3-before-bind-request', 'ta-a3', actor.actor, 'a3-before-request', 'reply:a3-before-request', false);
  assertUnbound(await bindingSnapshot(options, 'ta-a3'), 'TA-A3 after no-bind calls');
  const session = await bindActor(options, actor.actor);
  try {
    assertBound(await bindingSnapshot(options, 'ta-a3'), 'TA-A3 after bind');
    await assertCall(options, 'TA-A3-after-bind-send', 'ta-a3', actor.actor, 'a3-send', 'sent', true);
    await assertCall(options, 'TA-A3-after-bind-request', 'ta-a3', actor.actor, 'a3-request', 'reply:a3-request', false);
    await assertBoundPush(session, 'TA-A3-after-bind-push', 'ta-a3', 'a3-push');
  } finally { await session.close(); }
  const evidence = await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`);
  for (const [scenario, kind] of [
    ['TA-A3-before-bind-send', 'send'], ['TA-A3-before-bind-request', 'request'],
    ['TA-A3-after-bind-send', 'send'], ['TA-A3-after-bind-request', 'request'], ['TA-A3-after-bind-push', 'push']
  ] as const) requireEvidence(evidence, scenario, kind);
  console.log('scenario TA-A3 passed');
}
