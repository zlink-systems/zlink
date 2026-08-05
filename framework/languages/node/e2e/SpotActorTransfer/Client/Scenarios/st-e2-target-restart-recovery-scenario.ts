// ST-E2: Failed relocation keeps binding 시나리오를 검증한다.
import { SpotActorTransferNames, options, nodeA, nodeB, connectAndBind, assertBoundPush, assertHttpBoundPush, createSpot, createActor, joinActor, getEvidence, has, unique, uniqueShort, require } from '../Support/scenario-support';

export async function runStE2(): Promise<void> {
  const actorId = uniqueShort('e2');
  const spotId = unique('spot-bound-failed-transfer');
  await createSpot(nodeB, spotId);
  const source = await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeFailTransferOut, 92);
  const transferId = uniqueShort('transfer');
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'ST-E2', source, transferId);
  try {
    require(!(await joinActor(nodeA, actorId, { scenario: 'ST-E2', targetSpotId: spotId, transferId })).accepted, 'ST-E2 failed transfer returned success.');
    await assertBoundPush(connector, nodeA, actorId, 'ST-E2', 'after-failed-transfer', 'actor-a');
    const targetEvidence = await getEvidence(nodeB);
    require(!has(targetEvidence, actorId, 'bound_push'), 'ST-E2 target bound route was exposed.');
  } finally {
    await connector.close();
  }

  const rollbackActorId = uniqueShort('e2-joined');
  const rollbackSpotId = unique('spot-bound-joined-failure');
  await createSpot(nodeB, rollbackSpotId, 'fail-joined');
  const rollbackSource = await createActor(nodeA, rollbackActorId, SpotActorTransferNames.actorTypeStateful, 93);
  const rollbackTransferId = uniqueShort('transfer');
  const rollbackConnector = await connectAndBind(
    options.sessionAStreamEndpoint,
    'ST-E2',
    rollbackSource,
    rollbackTransferId
  );
  try {
    require(!(
      await joinActor(nodeA, rollbackActorId, {
        scenario: 'ST-E2',
        targetSpotId: rollbackSpotId,
        transferId: rollbackTransferId
      })
    ).accepted, 'ST-E2 joined failure returned success.');
    await assertHttpBoundPush(
      rollbackConnector,
      nodeB,
      rollbackActorId,
      'ST-E2',
      'after-joined-failure',
      'actor-a'
    );
    require(!has(await getEvidence(nodeB), rollbackActorId, 'bound_push'), 'ST-E2 rolled-back target exposed a bound route.');
  } finally {
    await rollbackConnector.close();
  }
}
