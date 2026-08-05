// ST-H4: Invalid context and duplicate registration 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  actorNode,
  createActor,
  createRemoteSpot,
  getEvidence,
  joinActor,
  nodeA,
  require,
  unique,
  waitActorRef,
  waitEvidence
} from '../Support/scenario-support';

export async function runStH4(): Promise<void> {
  const actorId = unique('actor-h4');
  const actor = await createActor(
    nodeA,
    actorId,
    SpotActorTransferNames.actorTypeStateful,
    204
  );
  const source = actorNode(actor.nodeRid);
  const targetSpot = await createRemoteSpot(actor.nodeRid);
  const target = actorNode(targetSpot.nodeRid);

  require((await joinActor(source, actorId, {
    scenario: 'ST-H4',
    targetSpotId: targetSpot.spotId
  })).accepted, 'ST-H4 primary deferred Join failed.');

  await waitEvidence(source, [
    `ST-H4|${actorId}|duplicate_defer_rejected|`,
    `ST-H4|${actorId}|pending_transition_rejected|`
  ]);
  await waitEvidence(target, [
    `ST-H4|${actorId}|join_completion|accepted|`
  ]);

  const evidence = await getEvidence(source);
  require(
    evidence.some(entry =>
      entry.actorId === actorId
      && entry.kind === 'duplicate_defer_rejected'
      && entry.value === 'alreadySubmitted'
    ),
    'ST-H4 duplicate Defer did not fail with alreadySubmitted.'
  );
  require(
    evidence.some(entry =>
      entry.actorId === actorId
      && entry.kind === 'pending_transition_rejected'
      && entry.value === 'actorMoving'
    ),
    'ST-H4 second pending transition did not fail with actorMoving.'
  );

  const moved = await waitActorRef(source, actorId, targetSpot.nodeRid);
  require(
    moved.objectGeneration === actor.objectGeneration,
    'ST-H4 changed ObjectGeneration during the accepted primary Join.'
  );
}
