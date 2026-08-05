// ST-A2: Local Join reject 시나리오를 검증한다.
import { SpotActorTransferNames, nodeA, createSpot, createActor, joinActor, probeActor, getRef, getEvidence, waitEvidence, has, unique, require } from '../Support/scenario-support';

export async function runStA2(): Promise<void> {
  const actorId = unique('actor-local-reject');
  const spotId = unique('spot-local-reject');
  await createSpot(nodeA, spotId, 'reject');
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 12);
  const join = await joinActor(nodeA, actorId, { scenario: 'ST-A2', targetSpotId: spotId, expectedMode: 'reject' });
  require(!join.accepted, 'ST-A2 join should be rejected.');
  const entries = await waitEvidence(nodeA, [`ST-A2|${actorId}|admission|spot=${spotId}`]);
  require(!has(entries, actorId, 'leave'), 'ST-A2 source leave side effect exists.');
  require(!has(entries, actorId, 'joined'), 'ST-A2 target joined side effect exists.');
  const actorRef = await getRef(nodeA, actorId);
  require(actorRef.nodeRid === 'actor-a', 'ST-A2 source ownership changed.');
  const entryProbe = await probeActor(nodeA, actorId, 'ST-A2', 'after-reject');
  require(entryProbe.nodeRid === 'actor-a', 'ST-A2 Entry Spot actor packet failed after rejection.');
  const afterProbe = await getEvidence(nodeA);
  require(has(afterProbe, actorId, 'entry_packet_handler'), 'ST-A2 Entry Spot handler evidence is missing.');
  require(!has(afterProbe, actorId, 'packet_handler'), 'ST-A2 target user Spot handled a rejected actor.');
}
