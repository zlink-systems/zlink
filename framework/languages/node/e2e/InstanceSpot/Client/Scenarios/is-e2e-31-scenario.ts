// IS-E2E-31: Remote selection loser 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-31' as const;

export async function runISE2E31(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-31-spot';
  const operationId = 'is-e2e-31-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

