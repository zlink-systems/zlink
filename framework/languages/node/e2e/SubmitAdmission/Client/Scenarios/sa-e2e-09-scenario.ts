// SA-E2E-09: RouteMesh Channel send deadline을 적용한다 시나리오를 검증한다.
import {
  emit,
  ensure,
  submitChannel,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-09' as const;

export async function runSAE2E09(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa09-channel';
  terminal(await submitChannel(context, operationId), operationId);
  const evidence = await waitEvidence(context, operationId, (value) => value.completed === 1);
  ensure(evidence.entered === 1, 'SA-E2E-09 channel handler count mismatch');
  emit(context, { status: 'submitted', selectedTarget: context.targetRid });
}
