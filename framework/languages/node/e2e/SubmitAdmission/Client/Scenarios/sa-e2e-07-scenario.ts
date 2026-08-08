// SA-E2E-07: Admission terminal과 publish commit을 구분한다 시나리오를 검증한다.
import {
  emit,
  submitFanout,
  terminal,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-07' as const;

export async function runSAE2E07(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa07-fanout';
  terminal(await submitFanout(context, operationId), operationId);
  emit(context, { status: 'submitted', subscriberCount: 0, handlerCount: 0 });
}
