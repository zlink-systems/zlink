// IS-E2E-13: Accepted send then failure 시나리오를 검증한다.
import { sendInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-13' as const;

export async function runISE2E13(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-13-spot';
  const operationId = 'is-e2e-13-' + Date.now();
  await sendInstance(context, spotId, operationId, 'accepted-send');
  await waitForInstanceEvidence(context, operationId);
}

