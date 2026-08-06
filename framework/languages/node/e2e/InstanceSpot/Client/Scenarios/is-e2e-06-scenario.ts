// IS-E2E-06: Creating owner crash는 같은 generation의 recovery 경계를 지킨다 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-06' as const;

export async function runISE2E06(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-06-spot';
  const operationId = 'is-e2e-06-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

