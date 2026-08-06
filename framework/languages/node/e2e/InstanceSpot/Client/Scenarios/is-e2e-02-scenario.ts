// IS-E2E-02: Cold send 시나리오를 검증한다.
import { sendInstance, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-02' as const;

export async function runISE2E02(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'is-e2e-02-spot';
  const operationId = 'is-e2e-02-' + Date.now();
  await sendInstance(context, spotId, operationId, context.scenarioId);
  await waitForInstanceEvidence(context, operationId);
}

