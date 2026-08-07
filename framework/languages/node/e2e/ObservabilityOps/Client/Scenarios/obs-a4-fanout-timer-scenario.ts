// OBS-A4: Fanout payload를 전달하고 timer는 새 flow를 만든다 시나리오를 검증한다.
import { post, require, unique, workflowA, workflowB } from '../Support/scenario-support.js';
import { readFlowRecords, waitFor, waitForFlow } from '../Support/observability-support.js';
import type { ActorEvidence, WorkflowApplyRes } from '../../Shared/messages.js';

export async function runObsA4(): Promise<void> {
  const orderId = unique('obs-a4-order');
  const result = await post<WorkflowApplyRes>(workflowA, '/workflows', { orderId, value: 1 });
  require(result.value === 2, 'OBS-A4 workflow state did not apply the event.');
  for (const client of [workflowA, workflowB]) {
    await waitFor(async () => await client.get('/evidence').fetch<ActorEvidence[]>(),
      (entries) => entries.some((entry) => entry.scenario === 'projection' && entry.actorId === orderId),
      `OBS-A4 projection was not received by ${client === workflowA ? 'workflow-a' : 'workflow-b'}`);
  }
  await waitForFlow([workflowA, workflowB], 'WorkflowProjected');
  const timerRecord = await waitFor(async () => (await readFlowRecords(workflowA))
    .find((record) => record.flow_origin === 'Timer'),
  (value) => value !== undefined, 'OBS-A4 timer did not originate a new flow');
  require(typeof timerRecord?.flow_id === 'string', 'OBS-A4 timer record did not include a flow.');
}
