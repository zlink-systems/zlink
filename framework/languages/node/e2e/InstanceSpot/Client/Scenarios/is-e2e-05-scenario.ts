// IS-E2E-05: Ready owner crash는 자동 takeover로 이어지지 않는다 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-05' as const;

export async function runISE2E05(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-05-spot';
  const operationId = 'is-e2e-05-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

