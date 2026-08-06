// IS-E2E-03: Concurrent first call 시나리오를 검증한다.
import { runConcurrentRequests, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-03' as const;

export async function runISE2E03(context: InstanceScenarioContext): Promise<void> {
  const spotId = 'shared-instance-spot';
  const operationIds = await runConcurrentRequests(context, spotId, 4, context.scenarioId);
  for (const operationId of operationIds) await waitForInstanceEvidence(context, operationId);
}

