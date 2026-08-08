// OBS-A3: Tracing off 구간은 inbound flow를 전파하지 않는다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  assertBoundPush,
  connectAndBind,
  createActor,
  createSpot,
  joinActor,
  nodeA,
  nodeB,
  options,
  require,
  session,
  unique
} from '../Support/scenario-support.js';
import { readFlowRecords, waitFor } from '../Support/observability-support.js';
import type { ActorRefSnapshotRes } from '../../Shared/messages.js';

export async function runObsA3(): Promise<void> {
  const actorId = unique('obs-a3-actor');
  const spotId = unique('obs-a3-room');
  const spot = await createSpot(nodeA, spotId);
  const actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 3);
  require((await joinActor(nodeA, actorId, { scenario: 'OBS-A3', targetSpotId: spotId })).accepted,
    'OBS-A3 actor join failed.');
  const roomNode = spot.nodeRid === 'play-a' ? nodeA : nodeB;
  const finalRef = await waitFor<ActorRefSnapshotRes | undefined>(async () => {
    try {
      return await roomNode.get(`/actors/${actorId}/ref`).fetch<ActorRefSnapshotRes>();
    } catch {
      return undefined;
    }
  }, (value) => value?.nodeRid === spot.nodeRid, 'OBS-A3 Actor authority did not reach the room node.');
  require(finalRef !== undefined, 'OBS-A3 Actor reference was not returned.');
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'OBS-A3', {
    ...actor,
    nodeRid: finalRef.nodeRid,
    objectGeneration: finalRef.objectGeneration,
    meshName: finalRef.meshName
  }, unique('off-bind'));
  try {
    await assertBoundPush(connector, roomNode, actorId, 'OBS-A3', 'through-off-node', spot.nodeRid);
  } finally {
    await connector.close();
  }
  await waitFor(async () => await readFlowRecords(roomNode),
    (value) => value.some((record) => record.packet_name === 'BoundPushReq'
      && typeof record.flow_id === 'string' && /^[0-9a-f-]{36}$/.test(record.flow_id)),
    'OBS-A3 downstream Play did not receive the propagated flow');
  require(!(await readFlowRecords(session)).some((record) => record.packet_name === 'BoundPushReq'),
    'OBS-A3 tracing-off Session emitted a flow line.');
}
