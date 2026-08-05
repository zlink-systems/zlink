// OBS-B4: Metric reader가 없어도 messaging을 처리한다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  createActor,
  createSpot,
  joinActor,
  nodeA,
  probeActor,
  require,
  unique
} from '../Support/scenario-support.js';
import { metrics, readFlowLog } from '../Support/observability-support.js';

export async function runObsB4(): Promise<void> {
  const actorId = unique('obs-b4-actor');
  const spotId = unique('obs-b4-room');
  await createSpot(nodeA, spotId);
  await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 4);
  require((await joinActor(nodeA, actorId, { scenario: 'OBS-B4', targetSpotId: spotId })).accepted,
    'OBS-B4 actor join failed with metrics disabled.');
  require((await probeActor(nodeA, actorId, 'OBS-B4', 'metrics-off')).marker === 'metrics-off',
    'OBS-B4 messaging changed with metrics disabled.');
  require((await metrics(nodeA)).length === 0, 'OBS-B4 disabled reader retained metric values.');
  require((await readFlowLog(nodeA)).includes('packet=ProbeReq '),
    'OBS-B4 metrics-off traffic did not traverse the framework.');
}
