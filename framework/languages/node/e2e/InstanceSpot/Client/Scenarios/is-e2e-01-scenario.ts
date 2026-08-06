// IS-E2E-01: Cold request 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-01' as const;

export async function runISE2E01(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-01-spot';
  const operationId = 'is-e2e-01-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

