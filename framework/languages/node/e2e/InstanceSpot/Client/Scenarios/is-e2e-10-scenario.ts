// IS-E2E-10: Stale owner resume 뒤에도 자동 owner가 생기지 않는다 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-10' as const;

export async function runISE2E10(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-10-spot';
  const operationId = 'is-e2e-10-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

