// ST-D2: Stale source message fencing 시나리오를 검증한다.
import { SpotActorTransferNames, nodeA, nodeB, createSpot, createActor, joinActor, probeActor, getRef, unique, delay, require } from '../Support/scenario-support';

export async function runStD2(): Promise<void> {
  const actorId = unique('actor-stale-release');
  const spotId = unique('spot-stale-release');
  await createSpot(nodeB, spotId);
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 82);
  require((await joinActor(nodeA, actorId, { scenario: 'ST-D2', targetSpotId: spotId })).accepted, 'ST-D2 join failed.');
  const before = await getRef(nodeB, actorId);
  await delay(1000);
  const after = await getRef(nodeB, actorId);
  require(
    after.nodeRid === 'actor-b'
      && after.objectGeneration === before.objectGeneration,
    'ST-D2 stale source release removed target generation.'
  );
  require((await probeActor(nodeB, actorId, 'ST-D2', 'after-stale-release')).nodeRid === 'actor-b', 'ST-D2 follow-up route failed.');
}
