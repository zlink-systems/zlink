// OBS-C2: Actor handoff 뒤 bound Session push를 유지한다 시나리오를 검증한다.
import {
  ObservabilityOpsNames,
  assertBoundPush,
  assertHttpBoundPush,
  connectAndBind,
  createActor,
  nodeA,
  nodeB,
  options,
  require,
  unique
} from '../Support/scenario-support.js';
import { metric, metrics, retireCompleted, startDrain, waitFor, waitForDrain } from '../Support/observability-support.js';
import type { ActorRefSnapshotRes } from '../../Shared/messages.js';

export async function runObsC2(): Promise<void> {
  const actorId = unique('obs-c2-actor');
  const actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 2);
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'OBS-C2', actor, unique('c2-bind'));
  await assertBoundPush(connector, nodeA, actorId, 'OBS-C2', 'before-drain', 'play-a');
  await startDrain(nodeA, 15000);
  const complete = await waitForDrain(nodeA,
    retireCompleted,
    'OBS-C2 drain did not complete', 15000);
  await waitFor(async () => {
    try {
      return await nodeB.get(`/actors/${actorId}/ref`).fetch<ActorRefSnapshotRes>();
    } catch {
      return undefined;
    }
  }, (actorRef) => actorRef?.nodeRid === 'play-b', 'OBS-C2 actor was not handed off to play-b', 15000);
  require(retireCompleted(complete), 'OBS-C2 retire did not complete.');
  await assertHttpBoundPush(connector, nodeB, actorId, 'OBS-C2', 'after-drain', 'play-b');
  require(metric(await metrics(nodeA), 'zlink.drain.actors.handed_off').value >= 1,
    'OBS-C2 handed-off metric was not incremented.');
  await connector.close();
}
