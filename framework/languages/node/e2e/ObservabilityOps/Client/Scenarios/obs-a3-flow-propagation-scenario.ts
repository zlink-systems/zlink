// OBS-A3: Tracing off 구간은 inbound flow를 전파하지 않는다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  assertBoundPush,
  connectAndBind,
  createActor,
  createSpot,
  joinActor,
  nodeA,
  options,
  require,
  session,
  unique
} from '../Support/scenario-support.js';
import { readFlowLog, waitFor } from '../Support/observability-support.js';

export async function runObsA3(): Promise<void> {
  const actorId = unique('obs-a3-actor');
  const spotId = unique('obs-a3-room');
  await createSpot(nodeA, spotId);
  const actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 3);
  require((await joinActor(nodeA, actorId, { scenario: 'OBS-A3', targetSpotId: spotId })).accepted,
    'OBS-A3 actor join failed.');
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'OBS-A3', actor, unique('off-bind'));
  try {
    await assertBoundPush(connector, nodeA, actorId, 'OBS-A3', 'through-off-node', 'play-a');
  } finally {
    await connector.close();
  }
  await waitFor(async () => await readFlowLog(nodeA),
    (value) => value.includes('packet=BoundPushReq ') && /flow=[0-9a-f-]{36}/.test(value),
    'OBS-A3 downstream Play did not receive the propagated flow');
  require(!(await readFlowLog(session)).includes('packet=BoundPushReq '),
    'OBS-A3 tracing-off Session emitted a flow line.');
}
