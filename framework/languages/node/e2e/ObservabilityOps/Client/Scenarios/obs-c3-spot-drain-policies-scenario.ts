// OBS-C3: User Spot aggregate와 member Actor를 함께 이동한다 시나리오를 검증한다.
import {
  createSpot,
  nodeA,
  post,
  require,
  unique,
  workflowA,
  workflowB
} from '../Support/scenario-support.js';
import { retireCompleted, startDrain, waitForDrain } from '../Support/observability-support.js';
import type { WorkflowApplyRes } from '../../Shared/messages.js';

export async function runObsC3(): Promise<void> {
  const roomRid = unique('obs-c3-room');
  await createSpot(nodeA, roomRid);
  await startDrain(nodeA, 15000);
  const natural = await waitForDrain(nodeA, (status) => !status.ready && status.result === undefined,
    'OBS-C3 room did not remain during natural drain');
  require(natural.result === undefined, 'OBS-C3 natural room closed before application release.');
  await post(nodeA, `/spots/${roomRid}/close`, {});
  await waitForDrain(nodeA, retireCompleted, 'OBS-C3 natural retire did not finish');

  const orderId = unique('obs-c3-order');
  const initial = await post<WorkflowApplyRes>(workflowA, '/workflows', { orderId, value: 5 });
  require(initial.value === 10, 'OBS-C3 initial workflow value mismatch.');
  await startDrain(workflowA, 10000);
  await waitForDrain(workflowA, retireCompleted,
    'OBS-C3 release-and-recreate drain did not finish');
  const replayed = await post<WorkflowApplyRes>(workflowB, '/workflows', { orderId, value: 2 });
  require(replayed.nodeRid === 'workflow-b' && replayed.replayed && replayed.value === 12,
    'OBS-C3 workflow was not replayed on workflow-b.');
}
