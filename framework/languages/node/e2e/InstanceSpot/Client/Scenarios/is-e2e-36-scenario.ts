// IS-E2E-36: First handler terminal recovery 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-36' as const;

export async function runISE2E36(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-36-spot';
  const operationId = 'is-e2e-36-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

