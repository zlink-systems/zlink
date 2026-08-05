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
  const spotId = unique('obs-b2-room');
  await createSpot(nodeB, spotId);
  await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 2);
  require((await joinActor(nodeA, actorId, { scenario: 'OBS-B2', targetSpotId: spotId })).accepted,
    'OBS-B2 actor transfer failed.');
  require((await probeActor(nodeB, actorId, 'OBS-B2', 'after-transfer')).nodeRid === 'play-b',
    'OBS-B2 actor did not reach play-b.');
  const source = await waitFor(async () => await metrics(nodeA),
    (values) => values.some((value) => value.name === 'zlink.actor.transfers' && value.value >= 1),
    'OBS-B2 actor transfer metric was not recorded');
  require(metric(source, 'zlink.actor.transfers').value >= 1, 'OBS-B2 transfer counter mismatch.');
  metric(source, 'zlink.actor.transfer.duration');
  metric(source, 'zlink.actor.transfer.pending_requests.count');
}
