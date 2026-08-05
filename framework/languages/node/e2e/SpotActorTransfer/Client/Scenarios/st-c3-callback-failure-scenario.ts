// ST-C3: Application callback failure 시나리오를 검증한다.
import { SpotActorTransferNames, nodeA, nodeB, assertSourceFailure, createSpot, createActor, joinActor, getEvidence, has, unique, require } from '../Support/scenario-support';

export async function runStC3(): Promise<void> {
  await assertSourceFailure('transfer-out', SpotActorTransferNames.actorTypeFailTransferOut, 'transfer_out_failed');
  await assertSourceFailure(
    'leave',
    SpotActorTransferNames.actorTypeFailLeave,
    'leave_failed',
    true
  );

  const transferInId = unique('actor-fail-transfer-in');
  const transferInSpot = unique('spot-fail-transfer-in');
  await createSpot(nodeB, transferInSpot);
  await createActor(nodeA, transferInId, SpotActorTransferNames.actorTypeFailTransferIn, 73);
  require(!(await joinActor(nodeA, transferInId, { scenario: 'ST-C3', targetSpotId: transferInSpot })).accepted, 'ST-C3 transferIn failure returned success.');
  const targetAfterTransferIn = await getEvidence(nodeB);
  require(has(targetAfterTransferIn, transferInId, 'transfer_in_failed'), 'ST-C3 transferIn failure evidence missing.');
  require(!has(targetAfterTransferIn, transferInId, 'joined'), 'ST-C3 joined ran after transferIn failure.');

  const joinedId = unique('actor-fail-joined');
  const joinedSpot = unique('spot-fail-joined');
  await createSpot(nodeB, joinedSpot, 'fail-joined');
  await createActor(nodeA, joinedId, SpotActorTransferNames.actorTypeStateful, 74);
  require(!(await joinActor(nodeA, joinedId, { scenario: 'ST-C3', targetSpotId: joinedSpot })).accepted, 'ST-C3 joined failure returned success.');
  const targetAfterJoined = await getEvidence(nodeB);
  require(has(targetAfterJoined, joinedId, 'joined_failed'), 'ST-C3 joined failure evidence missing.');
}
