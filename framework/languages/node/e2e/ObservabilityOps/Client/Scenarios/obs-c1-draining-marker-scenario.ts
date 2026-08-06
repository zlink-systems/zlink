// OBS-C1: Relocating host를 신규 placement에서 제외한다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  createActor,
  nodeA,
  nodeB,
  post,
  require,
  unique
} from '../Support/scenario-support.js';
import { retireCompleted, startDrain, waitForDrain } from '../Support/observability-support.js';

export async function runObsC1(): Promise<void> {
  let actorId = `actor-handoff-gate-${unique('obs-c1')}`;
  let actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 1);
  for (let attempt = 0; attempt < 8 && actor.nodeRid !== 'play-a'; attempt += 1) {
    actorId = `actor-handoff-gate-${unique('obs-c1')}`;
    actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 1);
  }
  require(actor.nodeRid === 'play-a', `OBS-C1 source actor was placed on '${actor.nodeRid}'.`);
  await startDrain(nodeA, 30000);
  const draining = await waitForDrain(nodeB,
    (status) => status.peerRows?.some((row) => row.nodeRid === 'play-a' && row.draining) === true,
    'OBS-C1 typed draining row was not observable');
  require(draining.result === undefined, 'OBS-C1 drain completed before the marker could be observed.');

  const fresh = await createActor(nodeB, unique('obs-c1-new'), ObservabilityOpsNames.actorTypeStateful, 0);
  require(fresh.nodeRid === 'play-b', `OBS-C1 new actor was placed on '${fresh.nodeRid}'.`);
  await post(nodeB, '/evidence/wait', {
    containsAll: [`OBS-C1|${actorId}|restore_gate|1`],
    timeoutMilliseconds: 10000
  });
  await post(nodeB, `/transfer-gates/${actorId}/release`, {});
  await waitForDrain(nodeA, retireCompleted, 'OBS-C1 retire did not complete');
}
