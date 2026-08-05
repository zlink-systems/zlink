// ST-E1A: New Actor incarnation requires bind 시나리오를 검증한다.
import type { BoundPushNotify } from '../../Shared/messages.js';
import {
  SpotActorTransferNames,
  options,
  nodeA,
  actorNode,
  connectAndBind,
  assertHttpBoundPush,
  createActor,
  destroyActor,
  post,
  waitActorRef,
  waitEvidence,
  delay,
  require,
  uniqueShort
} from '../Support/scenario-support';

export async function runStE1A(): Promise<void> {
  const scenario = 'ST-E1A';
  const actorId = uniqueShort('e1a');
  const original = await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 93);
  const originalNode = actorNode(original.nodeRid);
  const oldConnector = await connectAndBind(
    options.sessionAStreamEndpoint,
    scenario,
    original,
    uniqueShort('bind-old')
  );
  try {
    await assertHttpBoundPush(oldConnector, originalNode, actorId, scenario, 'before-destroy', original.nodeRid);
    require(await destroyActor(originalNode, actorId), 'ST-E1A original Actor was not destroyed.');

    const recreated = await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 94);
    const recreatedNode = actorNode(recreated.nodeRid);
    await waitActorRef(nodeA, actorId, recreated.nodeRid);
    require(
      recreated.objectGeneration !== original.objectGeneration,
      'ST-E1A recreated Actor reused the previous object generation.'
    );

    let oldBindingReceivedPush = false;
    const oldBindingWait = oldConnector
      .waitFor<BoundPushNotify>(SpotActorTransferNames.packetBoundNotify)
      .where((message) => message.payload.scenario === scenario && message.payload.marker === 'before-rebind')
      .timeout(1000)
      .submit()
      .then(() => { oldBindingReceivedPush = true; }, () => undefined);
    await post(recreatedNode, `/actors/${actorId}/bound-push`, { scenario, marker: 'before-rebind' });
    await waitEvidence(recreatedNode, [
      `${scenario}|${actorId}|bound_push_rejected|10`
    ]);
    await delay(1200);
    await oldBindingWait;
    require(!oldBindingReceivedPush, 'ST-E1A recreated Actor pushed through the old binding.');

    const newConnector = await connectAndBind(
      options.sessionAStreamEndpoint,
      scenario,
      recreated,
      uniqueShort('bind-new')
    );
    try {
      let oldBindingReceivedAfterRebind = false;
      const oldBindingAfterRebindWait = oldConnector
        .waitFor<BoundPushNotify>(SpotActorTransferNames.packetBoundNotify)
        .where((message) => message.payload.scenario === scenario && message.payload.marker === 'after-rebind')
        .timeout(1000)
        .submit()
        .then(() => { oldBindingReceivedAfterRebind = true; }, () => undefined);
      await assertHttpBoundPush(newConnector, recreatedNode, actorId, scenario, 'after-rebind', recreated.nodeRid);
      await waitEvidence(recreatedNode, [
        `${scenario}|${actorId}|bound_push|after-rebind`
      ]);
      await oldBindingAfterRebindWait;
      require(
        !oldBindingReceivedAfterRebind,
        'ST-E1A recreated Actor pushed through the old binding after rebind.'
      );
    } finally {
      await newConnector.close();
    }
  } finally {
    await oldConnector.close();
  }
}
