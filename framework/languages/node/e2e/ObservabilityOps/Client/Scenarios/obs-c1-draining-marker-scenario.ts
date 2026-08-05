// OBS-C1: Relocating host를 신규 placement에서 제외한다 시나리오를 검증한다.
import type { BoundPushNotify, BoundPushReq, BoundPushRes } from '../../Shared/messages.js';
import {
  ObservabilityOpsNames,
  connectAndBind,
  createActor,
  createSpot,
  nodeA,
  nodeB,
  options,
  post,
  require,
  unique
} from '../Support/scenario-support.js';
import { retireCompleted, startDrain, waitForDrain } from '../Support/observability-support.js';

export async function runObsC1(): Promise<void> {
  const actorId = `actor-handoff-gate-${unique('obs-c1')}`;
  const actor = await createActor(nodeA, actorId, ObservabilityOpsNames.actorTypeStateful, 1);
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'OBS-C1', actor, unique('c1-bind'));
  await startDrain(nodeA, 10000);
  const draining = await waitForDrain(nodeB,
    (status) => status.peerRows?.some((row) => row.nodeRid === 'play-a' && row.draining) === true,
    'OBS-C1 typed draining row was not observable');
  require(draining.result === undefined, 'OBS-C1 drain completed before the marker could be observed.');

  const push = connector.waitFor<BoundPushNotify>(ObservabilityOpsNames.packetBoundNotify)
    .where((message) => message.payload.actorId === actorId && message.payload.marker === 'in-flight')
    .timeout(10000).submit();
  const reply = connector.request({ scenario: 'OBS-C1', marker: 'in-flight' } satisfies BoundPushReq)
    .packetName(ObservabilityOpsNames.packetBoundPush).timeout(10000).submit<BoundPushRes>();
  const fresh = await createActor(nodeB, unique('obs-c1-new'), ObservabilityOpsNames.actorTypeStateful, 0);
  require(fresh.nodeRid === 'play-b', `OBS-C1 new actor was placed on '${fresh.nodeRid}'.`);
  await post(nodeA, `/transfer-gates/${actorId}/release`, {});
  require((await reply).actorId === actorId && (await push).payload.actorId === actorId,
    'OBS-C1 in-flight bound request was not preserved.');
  await waitForDrain(nodeA, retireCompleted, 'OBS-C1 retire did not complete');
  await connector.close();
}
