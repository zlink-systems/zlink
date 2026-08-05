// OBS-C5: Eligible target이 없으면 source를 유지한다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  createActor,
  createSpot,
  nodeA,
  nodeB,
  options,
  post,
  require,
  unique
} from '../Support/scenario-support.js';
import { retireCompleted, retireForceStopped, startDrain, waitFor, waitForDrain } from '../Support/observability-support.js';
import type { ActorRefSnapshotRes } from '../../Shared/messages.js';

export async function runObsC5(): Promise<void> {
  const actorId = unique('obs-c5-actor');
  await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 5);
  if (options.c5Phase === 'sequential') {
    await startDrain(nodeA, 10000);
    const result = await waitForDrain(nodeA,
      retireCompleted,
      'OBS-C5 sequential rollout did not drain play-a');
    await waitFor(async () => {
      try {
        return await nodeB.get(`/actors/${actorId}/ref`).fetch<ActorRefSnapshotRes>();
      } catch {
        return undefined;
      }
    }, (actor) => actor?.nodeRid === 'play-b', 'OBS-C5 sequential rollout did not hand off to play-b');
    require(retireCompleted(result), 'OBS-C5 sequential rollout force-stopped.');
    return;
  }
  const holdRid = unique('obs-c5-hold');
  await createSpot(nodeB, holdRid);
  await startDrain(nodeB, 5000);
  await waitForDrain(nodeB, (status) => !status.ready && status.result === undefined,
    'OBS-C5 target did not enter draining state');
  await startDrain(nodeA, 500);
  const source = await waitForDrain(nodeA, (status) => status.result !== undefined,
    'OBS-C5 zero-target source did not terminate');
  require(retireForceStopped(source)
    && source.result?.reason === 5,
    `OBS-C5 zero-target result was ${JSON.stringify(source.result)}.`);
  await post(nodeB, `/spots/${holdRid}/close`, {});
}
