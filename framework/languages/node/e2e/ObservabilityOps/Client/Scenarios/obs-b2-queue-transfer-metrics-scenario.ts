// OBS-B2: Actor relocation metric을 확인한다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  createActor,
  createSpot,
  joinActor,
  nodeA,
  nodeB,
  probeActor,
  require,
  unique
} from '../Support/scenario-support.js';
import { metric, metrics, waitFor } from '../Support/observability-support.js';

export async function runObsB2(): Promise<void> {
  const actorId = unique('obs-b2-actor');
  const actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 2);
  const sourceNode = actor.nodeRid === 'play-a' ? nodeA : nodeB;
  let target = await createSpot(nodeA, unique('obs-b2-room'));
  for (let attempt = 0; attempt < 8 && target.nodeRid === actor.nodeRid; attempt += 1) {
    target = await createSpot(nodeA, unique('obs-b2-room'));
  }
  require(target.nodeRid !== actor.nodeRid, 'OBS-B2 could not select a remote relocation target.');
  require((await joinActor(sourceNode, actorId, { scenario: 'OBS-B2', targetSpotId: target.spotId })).accepted,
    'OBS-B2 actor transfer failed.');
  const targetNode = target.nodeRid === 'play-a' ? nodeA : nodeB;
  const probe = await waitFor(async () => {
    try {
      return await probeActor(targetNode, actorId, 'OBS-B2', 'after-transfer');
    } catch {
      return undefined;
    }
  }, (value) => value?.nodeRid === target.nodeRid, 'OBS-B2 actor did not reach the selected remote node.');
  require(probe !== undefined, 'OBS-B2 actor probe did not return a result.');
  require(probe.nodeRid === target.nodeRid, 'OBS-B2 actor reached an unexpected node.');
  const snapshots = await waitFor(async () => await Promise.all([metrics(nodeA), metrics(nodeB)]),
    (values) => values.some((snapshot) => snapshot.some((value) =>
      value.name === 'zlink.relocation.completed'
      && value.tags.object_kind === 'actor'
      && value.tags.outcome === 'completed'
      && value.value >= 1)),
    'OBS-B2 actor relocation metric was not recorded');
  const source = snapshots.find((snapshot) => snapshot.some((value) =>
    value.name === 'zlink.relocation.completed'
    && value.tags.object_kind === 'actor'
    && value.tags.outcome === 'completed'));
  require(source !== undefined, 'OBS-B2 actor relocation source metrics were not found.');
  require(metric(source, 'zlink.relocation.started', (value) => value.tags.object_kind === 'actor').value >= 1,
    'OBS-B2 relocation start counter mismatch.');
  require(metric(source, 'zlink.relocation.completed', (value) => value.tags.object_kind === 'actor' && value.tags.outcome === 'completed').value >= 1,
    'OBS-B2 relocation completion counter mismatch.');
  metric(source, 'zlink.relocation.duration', (value) => value.tags.object_kind === 'actor');
  require(metric(source, 'zlink.relocation.bytes', (value) => value.tags.object_kind === 'actor').value > 0,
    'OBS-B2 relocation payload size was not recorded.');
}
