// SA-E2E-14: Subscriber가 없어도 classic fanout publish를 완료한다 시나리오를 검증한다.
import {
  emit,
  submitFanout,
  terminal,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-14' as const;

export async function runSAE2E14(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa14-subscriber-zero';
  terminal(await submitFanout(context, operationId), operationId);
  emit(context, { status: 'submitted', subscriberCount: 0 });
}
