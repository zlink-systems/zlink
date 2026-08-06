// SA-E2E-12: ActorId send의 admission과 logical identity를 유지한다 시나리오를 검증한다.
import {
  emit,
  ensure,
  submit,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-12' as const;

export async function runSAE2E12(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa12-actor';
  const result = await submit(context, context.callerUrl, operationId, context.targetRid);
  terminal(result, operationId);
  const evidence = await waitEvidence(context, operationId, (value) => value.completed === 1, context.targetUrl);
  ensure(evidence.entered === 1, `SA-E2E-12: handler count mismatch`);
  emit(context, { status: 'submitted', handlerCount: evidence.entered });
}
