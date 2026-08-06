// OBS-A1: STREAM에서 Actor와 room Spot까지 같은 flow를 유지한다 시나리오를 검증한다.
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
import { waitForFlow } from '../Support/observability-support.js';
import { waitFor } from '../Support/observability-support.js';
import type { ActorRefSnapshotRes } from '../../Shared/messages.js';

export async function runObsA1(): Promise<void> {
  const actorId = unique('obs-a1-actor');
  const spotId = unique('obs-a1-room');
  const spot = await createSpot(nodeA, spotId);
  const actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 1);
  require((await joinActor(nodeA, actorId, { scenario: 'OBS-A1', targetSpotId: spotId })).accepted,
    'OBS-A1 actor did not join the room Spot.');
  const roomNode = spot.nodeRid === 'play-a' ? nodeA : nodeB;
  const finalRef = await waitFor<ActorRefSnapshotRes | undefined>(async () => {
    try {
      return await roomNode.get(`/actors/${actorId}/ref`).fetch<ActorRefSnapshotRes>();
    } catch {
      return undefined;
    }
  }, (value) => value?.nodeRid === spot.nodeRid, 'OBS-A1 Actor authority did not reach the room node.');
  require(finalRef !== undefined, 'OBS-A1 Actor reference was not returned.');
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'OBS-A1', {
    ...actor,
    nodeRid: finalRef.nodeRid,
    objectGeneration: finalRef.objectGeneration,
    meshName: finalRef.meshName
  }, unique('flow-bind'));
  try {
    await assertBoundPush(
      connector,
      roomNode,
      actorId,
      'OBS-A1',
      'flow-through-room',
      spot.nodeRid
    );
  } finally {
    await connector.close();
  }
  const flow = await waitForFlow([session, roomNode], ObservabilityOpsNames.packetBoundPush);
  require(flow.length === 36, 'OBS-A1 did not preserve a UUIDv7 flow across roles.');
}
