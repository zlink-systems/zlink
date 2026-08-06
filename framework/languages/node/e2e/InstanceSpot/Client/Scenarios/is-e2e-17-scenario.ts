// IS-E2E-17: Activation backpressure 시나리오를 검증한다.
import { runConcurrentRequests, waitForInstanceEvidence, type InstanceScenarioContext } from '../Support/scenario-http';

export const scenarioId = 'IS-E2E-17' as const;

export async function runISE2E17(context: InstanceScenarioContext): Promise<void> {
  const operationIds = await runConcurrentRequests(context, 'is-e2e-17-spot', 3, 'bounded-activation');
  for (const operationId of operationIds) await waitForInstanceEvidence(context, operationId);
}

