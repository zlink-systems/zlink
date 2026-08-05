// ST-D1: Join completion과 current location 시나리오를 검증한다.
import { SpotActorTransferNames, nodeA, nodeB, createSpot, createActor, joinActor, getRef, waitEvidence, post, unique, delay, require } from '../Support/scenario-support';

export async function runStD1(): Promise<void> {
  const actorId = unique('actor-location-delay');
  const spotId = unique('spot-location-delay');
  await createSpot(nodeB, spotId, 'delay-joined');
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 81);
  const join = joinActor(nodeA, actorId, { scenario: 'ST-D1', targetSpotId: spotId });
  await waitEvidence(nodeB, [`ST-D1|${actorId}|joined_wait|${spotId}`]);
  const pending = await getRef(nodeA, actorId);
  require(pending.nodeRid === 'actor-a', 'ST-D1 target location became public before joined completed.');
  await post(nodeB, `/joined-gates/${spotId}/release`, {});
  require((await join).accepted, 'ST-D1 join failed.');
  const committed = await getRef(nodeB, actorId);
  require(committed.nodeRid === 'actor-b', 'ST-D1 committed target location is missing.');
}
