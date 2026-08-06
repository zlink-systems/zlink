// IS-E2E-12: Ambiguous result 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-12' as const;

export async function runISE2E12(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-12-spot';
  const operationId = 'is-e2e-12-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

