// SA-E2E-10: ClientServer Channel send deadline을 적용한다 시나리오를 검증한다.
import {
  emit,
  ensure,
  submitChannel,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-10' as const;

export async function runSAE2E10(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa10-channel';
  const result = await submitChannel(context, operationId);
  terminal(result, operationId);
  const evidence = await waitEvidence(context, operationId, (value) => value.completed === 1, context.targetUrl);
  ensure(evidence.entered === 1, `SA-E2E-10: handler count mismatch`);
  emit(context, { status: 'submitted', handlerCount: evidence.entered });
}
