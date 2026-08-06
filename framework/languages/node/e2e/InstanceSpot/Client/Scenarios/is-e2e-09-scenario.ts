// IS-E2E-09: Ready owner crash 뒤 concurrent request를 자동 전환하지 않는다 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-09' as const;

export async function runISE2E09(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-09-spot';
  const operationId = 'is-e2e-09-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

