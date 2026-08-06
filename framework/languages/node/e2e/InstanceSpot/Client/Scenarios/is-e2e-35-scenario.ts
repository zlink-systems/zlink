// IS-E2E-35: Ready owner crash 뒤 queue를 자동 복구하지 않는다 시나리오를 검증한다.
import { requestInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-35' as const;

export async function runISE2E35(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-35-spot';
  const operationId = 'is-e2e-35-' + Date.now();
  await requestInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

