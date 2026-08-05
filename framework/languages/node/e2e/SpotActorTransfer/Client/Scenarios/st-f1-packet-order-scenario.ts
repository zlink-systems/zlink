// ST-F1: In-flight handoff order 시나리오를 검증한다.
import { SpotActorTransferNames, nodeA, nodeB, createSpot, createActor, joinActor, sendHandoff, getEvidence, waitEvidence, post, assertValuesInOrder, has, unique, require } from '../Support/scenario-support';

export async function runStF1(): Promise<void> {
  const actorId = unique('actor-handoff-gate-f1');
  const spotId = unique('spot-handoff-order');
  await createSpot(nodeB, spotId);
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 101);
  const join = joinActor(nodeA, actorId, { scenario: 'ST-F1', targetSpotId: spotId });
  await waitEvidence(nodeA, [`ST-F1|${actorId}|before_commit_gate|101`]);
  for (const marker of ['P1', 'P2', 'P3']) await sendHandoff(nodeA, actorId, 'ST-F1', marker);
  await post(nodeA, `/transfer-gates/${actorId}/release`, {});
  require((await join).accepted, 'ST-F1 join failed.');
  const targetEntries = await waitEvidence(nodeB, ['P1', 'P2', 'P3'].map(
    (marker) => `ST-F1|${actorId}|packet_handler|${marker}`
  ));
  assertValuesInOrder(targetEntries, actorId, 'packet_handler', ['P1', 'P2', 'P3']);
  await waitEvidence(nodeA, [`ST-F1|${actorId}|source_cleanup|`]);
  require(!has(await getEvidence(nodeA), actorId, 'packet_handler'), 'ST-F1 source dispatched moving packets.');
}
