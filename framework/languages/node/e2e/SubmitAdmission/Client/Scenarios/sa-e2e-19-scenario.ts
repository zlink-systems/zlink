// SA-E2E-19: Terminal 뒤 route 복구가 operation을 재제출하지 않는다 시나리오를 검증한다.
import {
  emit,
  ensure,
  submit,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-19' as const;

export async function runSAE2E19(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa19-route-recovery';
  const result = await submit(context, context.callerUrl, operationId, context.targetRid);
  terminal(result, operationId);
  const evidence = await waitEvidence(context, operationId, (value) => value.completed === 1, context.targetUrl);
  ensure(evidence.entered === 1, `SA-E2E-19: handler count mismatch`);
  emit(context, { status: 'submitted', handlerCount: evidence.entered });
}
