// SA-E2E-08: Node direct의 local·remote send를 비교한다 시나리오를 검증한다.
import {
  emit,
  submit,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-08' as const;

export async function runSAE2E08(context: SubmitScenarioContext): Promise<void> {
  const localId = 'sa08-local';
  const remoteId = 'sa08-remote';
  terminal(await submit(context, context.callerUrl, localId, context.callerRid), localId);
  terminal(await submit(context, context.callerUrl, remoteId, context.targetRid), remoteId);
  await waitEvidence(context, localId, (value) => value.completed === 1, context.callerUrl);
  await waitEvidence(context, remoteId, (value) => value.completed === 1);
  emit(context, { localStatus: 'submitted', remoteStatus: 'submitted' });
}
