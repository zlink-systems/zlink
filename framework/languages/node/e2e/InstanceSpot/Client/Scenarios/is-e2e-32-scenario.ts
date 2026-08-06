// IS-E2E-32: Activation crash boundary 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-32' as const;

export async function runISE2E32(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-32-spot';
  const operationId = 'is-e2e-32-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

