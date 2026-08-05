// ST-H1: Deferred Join registration 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  actorNode,
  createActor,
  createRemoteSpot,
  getEvidence,
  joinActor,
  nodeA,
  post,
  require,
  sendHandoff,
  unique,
  waitActorRef,
  waitEvidence
} from '../Support/scenario-support';

export async function runStH1(): Promise<void> {
  const actorId = unique('actor-handoff-gate-h1');
  const actor = await createActor(
    nodeA,
    actorId,
    SpotActorTransferNames.actorTypeStateful,
    201
  );
  const source = actorNode(actor.nodeRid);
  const targetSpot = await createRemoteSpot(actor.nodeRid);
  const target = actorNode(targetSpot.nodeRid);
  const join = joinActor(source, actorId, {
    scenario: 'ST-H1',
    targetSpotId: targetSpot.spotId
  });

  const beforeCommit = await waitEvidence(source, [
    `ST-H1|${actorId}|join_deferred|${targetSpot.spotId}`,
    `ST-H1|${actorId}|request_mutated_after_defer|mutated-after-defer`,
    `ST-H1|${actorId}|before_commit_gate|201`
  ]);
  const ordered = beforeCommit
    .filter(entry => entry.actorId === actorId)
    .map(entry => entry.kind);
  require(
    ordered.indexOf('join_deferred') < ordered.indexOf('request_mutated_after_defer')
    && ordered.indexOf('request_mutated_after_defer') < ordered.indexOf('before_commit_gate'),
    'ST-H1 started relocation before the handler tail.'
  );

  await sendHandoff(source, actorId, 'ST-H1', 'queued-behind-barrier');
  await post(source, `/transfer-gates/${actorId}/release`, {});
  require((await join).accepted, 'ST-H1 deferred Join submission failed.');
  await waitEvidence(target, [
    `ST-H1|${actorId}|admission|spot=${targetSpot.spotId}`,
    `ST-H1|${actorId}|packet_handler|queued-behind-barrier`,
    `ST-H1|${actorId}|join_completion|accepted|`
  ]);
  const current = await waitActorRef(source, actorId, targetSpot.nodeRid);
  require(
    current.objectGeneration === actor.objectGeneration,
    'ST-H1 changed ObjectGeneration.'
  );
  require(
    !(await getEvidence(target)).some(entry =>
      entry.actorId === actorId
      && entry.kind === 'admission'
      && entry.value.includes('mutated-after-defer')
    ),
    'ST-H1 used the request object after Defer instead of its immutable snapshot.'
  );
}
