// ST-B3: RecreateOnRelocation 시나리오를 검증한다.
import { SpotActorTransferNames, nodeA, nodeB, createSpot, createActor, joinActor, probeActor, waitEvidence, assertOrder, mergeEvidence, unique, require } from '../Support/scenario-support';

export async function runStB3(): Promise<void> {
  const actorId = unique('actor-no-adapter');
  const spotId = unique('spot-no-adapter');
  await createSpot(nodeB, spotId);
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeNoAdapter, 31);
  require((await joinActor(nodeA, actorId, { scenario: 'ST-B3', targetSpotId: spotId })).accepted, 'ST-B3 join failed.');
  const probe = await probeActor(nodeB, actorId, 'ST-B3', 'after-default-empty-transfer');
  require(probe.stateVersion === 0, 'ST-B3 default target state must be empty/default.');
  const source = await waitEvidence(nodeA, [
    `transfer|${actorId}|transfer_out_empty_default|no-adapter`,
    `ST-B3|${actorId}|commit_request|after-source-leave`,
    `ST-B3|${actorId}|commit_ack|${spotId}`,
    `ST-B3|${actorId}|success_reply|${spotId}`
  ]);
  const target = await waitEvidence(nodeB, [
    `ST-B3|${actorId}|admission|spot=${spotId}`,
    `transfer|${actorId}|transfer_in_empty_default|actor-factory`,
    `transfer|${actorId}|joined|${spotId}:0`,
    `ST-B3|${actorId}|location_committed|node=actor-b|spot=${spotId}`
  ]);
  assertOrder(source, actorId, [
    'transfer_out_empty_default', 'leave', 'commit_request', 'commit_ack', 'success_reply'
  ]);
  assertOrder(target, actorId, [
    'admission', 'transfer_in_empty_default', 'joined', 'location_committed'
  ]);
  assertOrder(mergeEvidence(source, target), actorId, [
    'location_committed', 'commit_ack', 'success_reply'
  ]);
}
