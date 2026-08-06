// SA-E2E-13: Logical Multicast는 수락 가능한 target을 한 번 처리한다 시나리오를 검증한다.
import {
  emit,
  submitFanout,
  terminal,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-13' as const;

export async function runSAE2E13(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa13-multicast';
  terminal(await submitFanout(context, operationId), operationId);
  emit(context, { status: 'submitted', partialDelivery: false, subscriberCount: 0 });
}
