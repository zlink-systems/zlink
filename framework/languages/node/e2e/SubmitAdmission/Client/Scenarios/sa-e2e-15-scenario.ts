// SA-E2E-15: Bound Session과 Session Actor relay의 local·remote 결과를 비교한다 시나리오를 검증한다.
import {
  emit,
  submit,
  submitChannel,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-15' as const;

export async function runSAE2E15(context: SubmitScenarioContext): Promise<void> {
  const sessionId = 'sa15-session';
  const actorId = 'sa15-session-actor';
  terminal(await submit(context, context.callerUrl, sessionId, context.targetRid), sessionId);
  terminal(await submitChannel(context, actorId), actorId);
  await waitEvidence(context, sessionId, (value) => value.completed === 1);
  await waitEvidence(context, actorId, (value) => value.completed === 1);
  emit(context, { status: 'submitted', session: true, sessionActor: true });
}
