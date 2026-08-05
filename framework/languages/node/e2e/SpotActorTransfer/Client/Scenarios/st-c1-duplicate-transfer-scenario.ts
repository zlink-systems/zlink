// ST-C1: Location Store response loss 시나리오를 검증한다.
import { SpotActorTransferNames, nodeA, nodeB, createSpot, createActor, joinActor, getEvidence, waitEvidence, post, has, unique, delay, require } from '../Support/scenario-support';

export async function runStC1(): Promise<void> {
  const actorId = unique('actor-source-down-before-commit');
  const spotId = unique('spot-source-down-before-commit');
  await createSpot(nodeB, spotId);
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 62);
  const join = joinActor(nodeA, actorId, { scenario: 'ST-C1', targetSpotId: spotId }).catch(() => undefined);
  await waitEvidence(nodeB, [`ST-C1|${actorId}|admission|spot=${spotId}`]);
  await post(nodeA, '/crash', {});
  await delay(100);
  await post(nodeB, `/transfer-gates/${actorId}/release`, {});
  await join;
  await delay(31_000);
  const entries = await getEvidence(nodeB);
  require(!has(entries, actorId, 'joined'), 'ST-C1 target joined after source died before commit.');
  require(!has(entries, actorId, 'transfer_in'), 'ST-C1 transferIn ran without commit.');
}
