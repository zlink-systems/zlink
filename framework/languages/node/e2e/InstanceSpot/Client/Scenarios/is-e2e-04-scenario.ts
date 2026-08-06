// IS-E2E-04: Different Spot ID 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-04' as const;

export async function runISE2E04(context: InstanceScenarioContext): Promise<void> {
  const firstSpotId = 'instance-spot-a';
  const secondSpotId = 'instance-spot-b';
  const firstOperationId = 'is-e2e-04-a-' + Date.now();
  const secondOperationId = 'is-e2e-04-b-' + Date.now();
  await Promise.all([
    requestInstance(context, firstSpotId, firstOperationId, 'first-spot'),
    requestInstance(context, secondSpotId, secondOperationId, 'second-spot')
  ]);
  await waitForInstanceEvidence(context, firstOperationId);
  await waitForInstanceEvidence(context, secondOperationId);
}

