// IS-E2E-19: Ready ordering 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-19' as const;

export async function runISE2E19(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-19-spot';
  const firstOperationId = 'is-e2e-19-first-' + Date.now();
  const secondOperationId = 'is-e2e-19-second-' + Date.now();
  await requestInstance(context, spotId, firstOperationId, 'first');
  await requestInstance(context, spotId, secondOperationId, 'follow-up');
  await waitForInstanceEvidence(context, firstOperationId);
  await waitForInstanceEvidence(context, secondOperationId);
}

