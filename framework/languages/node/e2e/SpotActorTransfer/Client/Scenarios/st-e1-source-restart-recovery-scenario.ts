// ST-E1: Bound Session push after relocation 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  options,
  nodeA,
  connectAndBind,
  assertBoundPush,
  assertHttpBoundPush,
  actorNode,
  createRemoteSpot,
  createActor,
  joinActor,
  waitEvidence,
  waitSpotRef,
  uniqueShort,
  require
} from '../Support/scenario-support';

export async function runStE1(): Promise<void> {
  const actorId = uniqueShort('e1');
  const source = await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 91);
  const sourceNode = actorNode(source.nodeRid);
  const targetSpot = await createRemoteSpot(source.nodeRid);
  const targetNode = actorNode(targetSpot.nodeRid);
  await waitSpotRef(sourceNode, targetSpot.spotId, targetSpot.nodeRid);
  const transferId = uniqueShort('transfer');
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'ST-E1', source, transferId);
  try {
    await assertBoundPush(connector, sourceNode, actorId, 'ST-E1', 'before-transfer', source.nodeRid);
    require(
      (await joinActor(sourceNode, actorId, {
        scenario: 'ST-E1',
        targetSpotId: targetSpot.spotId,
        transferId
      })).accepted,
      'ST-E1 join failed.'
    );
    await waitEvidence(targetNode, [`ST-E1|${actorId}|join_completion|accepted|`]);
    await assertHttpBoundPush(
      connector,
      targetNode,
      actorId,
      'ST-E1',
      'after-transfer',
      targetSpot.nodeRid
    );
  } finally {
    await connector.close();
  }
}
