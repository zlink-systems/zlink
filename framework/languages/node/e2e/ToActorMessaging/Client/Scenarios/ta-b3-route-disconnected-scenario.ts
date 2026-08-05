// TA-B3: Current owner에 연결할 수 없으면 Unavailable로 끝난다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import type { ActorCallResponse, ActorRefPayload } from '../../Shared/messages';
import {
  type ActorEvidence, assertCall, assertFailure, ensureActor, requireEvidence, requireNoEvidence
} from '../Support/actor-scenario-support';

export async function runTaB3(options: ClientOptions): Promise<void> {
  const actor = await ensureActor(options, 'ta-b3');
  console.log('scenario-control TA-B3 disconnect-route');
  await waitForControl(options, 'route-disconnected');
  await waitForRouteState(options, actor.actor, 'disconnected');
  await assertFailure(options, 'TA-B3-route-not-connected-request', 'ta-b3', 'Unavailable', false, actor.actor);

  console.log('scenario-control TA-B3 restore-route');
  await waitForControl(options, 'route-restored');
  await waitForRouteState(options, actor.actor, 'connected');
  await assertCall(options, 'TA-B3-route-restored-request', 'ta-b3', actor.actor, 'restored', 'reply:restored', false);
  const evidence = await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`);
  requireNoEvidence(evidence, 'TA-B3-route-not-connected-request');
  requireEvidence(evidence, 'TA-B3-route-restored-request', 'request');
  console.log('scenario TA-B3 passed');
}

async function waitForRouteState(
  options: ClientOptions,
  actor: ActorRefPayload,
  expected: 'connected' | 'disconnected'
): Promise<void> {
  await waitUntil(async () => {
    const response = await postJson<ActorCallResponse>(`${options.callerUrl}/request`, {
      scenario: 'TA-B3-route-probe',
      actorId: actor.actorId,
      actor,
      value: 'probe'
    });
    return expected === 'disconnected'
      ? response.errorKind === 'Unavailable'
      : response.result === 'reply:probe' && response.errorKind === undefined;
  }, `route ${expected}`);
}

async function waitForControl(options: ClientOptions, name: 'route-disconnected' | 'route-restored'): Promise<void> {
  await waitUntil(async () => {
    const state = await getJson<{ readonly ready: boolean }>(`${options.callerUrl}/control/${name}`);
    return state.ready;
  }, name);
}

async function waitUntil(probe: () => Promise<boolean>, label: string): Promise<void> {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    if (await probe()) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`TA-B3 timed out waiting for ${label}.`);
}
