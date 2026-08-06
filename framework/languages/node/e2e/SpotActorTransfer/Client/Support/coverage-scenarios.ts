import { SpotActorTransferNames } from '../../Shared/messages';
import {
  actorNode,
  createActor,
  createRemoteSpot,
  delay,
  joinActor,
  nodeA,
  probeActor,
  require,
  unique,
  waitEvidence,
  waitSpotRef
} from './scenario-support';

/** Exercises the public actor relocation and probe surface for an ungrouped scenario. */
export async function runSpotActorCoverage(scenario: string): Promise<void> {
  const actorId = unique(scenario.toLowerCase());
  const sourceActor = await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 1);
  const sourceNode = actorNode(sourceActor.nodeRid);
  const targetSpot = await createRemoteSpot(sourceActor.nodeRid);
  const targetNode = actorNode(targetSpot.nodeRid);
  await waitSpotRef(sourceNode, targetSpot.spotId, targetSpot.nodeRid);
  const joined = await joinActor(sourceNode, actorId, { scenario, targetSpotId: targetSpot.spotId });
  require(joined.accepted, scenario + ' public relocation request was rejected.');
  await waitEvidence(targetNode, [scenario + '|' + actorId + '|join_completion|accepted|']);
  await delay(250);
  const probe = await probeActor(targetNode, actorId, scenario, 'after-transfer');
  require(probe.nodeRid === targetSpot.nodeRid && probe.stateVersion === 1, scenario + ' target probe mismatch.');
  console.log('scenario ' + scenario + ' passed');
}
