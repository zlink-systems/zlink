// ST-C2: Target connection failure 시나리오를 검증한다.
import { SpotActorTransferNames, options, nodeA, nodeB, connectAndBind, assertBoundPush, assertHttpBoundPush, createSpot, createActor, joinActor, getRef, post, unique, uniqueShort, delay, require } from '../Support/scenario-support';

export async function runStC2(): Promise<void> {
  const actorId = uniqueShort('c2');
  const spotId = unique('spot-source-down-after-commit');
  await createSpot(nodeB, spotId);
  const source = await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 61);
  const transferId = uniqueShort('transfer');
  const connector = await connectAndBind(options.sessionBStreamEndpoint, 'ST-C2', source, transferId);
  try {
    await assertBoundPush(connector, nodeA, actorId, 'ST-C2', 'bound-before-transfer', 'actor-a');
    require((await joinActor(nodeA, actorId, { scenario: 'ST-C2', targetSpotId: spotId, transferId })).accepted, 'ST-C2 join failed.');
    const before = await getRef(nodeB, actorId);
    await assertHttpBoundPush(connector, nodeB, actorId, 'ST-C2', 'bound-after-commit', 'actor-b');
    await post(nodeA, '/shutdown', {});
    await delay(1500);
    const after = await getRef(nodeB, actorId);
    require(
      after.nodeRid === 'actor-b'
        && after.objectGeneration === before.objectGeneration,
      'ST-C2 target generation changed.'
    );
    await assertHttpBoundPush(connector, nodeB, actorId, 'ST-C2', 'bound-after-source-down', 'actor-b');
  } finally {
    await connector.close();
  }
}
