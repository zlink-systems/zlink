// TA-B2: 같은 ActorId로 다시 만든 Actor가 새 direct message를 처리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import {
  type ActorEvidence, assertCall, ensureActor, requireEvidence
} from '../Support/actor-scenario-support';

export async function runTaB2(options: ClientOptions): Promise<void> {
  const previous = await ensureActor(options, 'ta-b2');
  await postJson(`${options.actorUrl}/actors/ta-b2/destroy`, {});
  const replacement = await ensureActor(options, 'ta-b2');
  if (replacement.actor.objectGeneration === previous.actor.objectGeneration) {
    throw new Error('TA-B2 actor recreation did not change object generation.');
  }

  const staleLifecycle = await postJson<{ readonly status: string; readonly errorKind?: string }>(
    `${options.actorUrl}/actors/ta-b2/destroy-ref`,
    previous.actor
  );
  if (staleLifecycle.status !== 'failed' || staleLifecycle.errorKind !== 'InvalidOperation') {
    throw new Error(`TA-B2 stale lifecycle expected InvalidOperation, got ${JSON.stringify(staleLifecycle)}.`);
  }
  await assertCall(options, 'TA-B2-id-send', 'ta-b2', previous.actor, 'id-send', 'sent', true);
  await assertCall(options, 'TA-B2-id-request', 'ta-b2', replacement.actor, 'id-request', 'reply:id-request', false);
  const evidence = await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`);
  requireEvidence(evidence, 'TA-B2-id-send', 'send');
  requireEvidence(evidence, 'TA-B2-id-request', 'request');
  console.log('scenario TA-B2 passed');
}
