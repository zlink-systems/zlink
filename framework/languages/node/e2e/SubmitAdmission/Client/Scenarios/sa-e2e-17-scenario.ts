// SA-E2E-17: Stream reply token은 한 번만 사용한다 시나리오를 검증한다.
import {
  emit,
  ensure,
  submit,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-17' as const;

export async function runSAE2E17(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa17-reply-token';
  const result = await submit(context, context.callerUrl, operationId, context.targetRid);
  terminal(result, operationId);
  const evidence = await waitEvidence(context, operationId, (value) => value.completed === 1, context.targetUrl);
  ensure(evidence.entered === 1, `SA-E2E-17: handler count mismatch`);
  emit(context, { status: 'submitted', handlerCount: evidence.entered });
}
