// IS-E2E-18: Cross-language 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-18' as const;

export async function runISE2E18(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-18-spot';
  const operationId = 'is-e2e-18-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

