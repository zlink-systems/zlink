import {
  SpotActorTransferNames,
  connectAndBind,
  createActor,
  delay,
  nodeA,
  options,
  require,
  uniqueShort
} from '../Support/scenario-support';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type { BoundPushNotify } from '../../Shared/messages';

/** Proves replacement terminal ordering and the non-blocking retired-session close. */
export async function runNodeSess001Process(): Promise<void> {
  const scenario = 'NODE-SESS-001';
  const actor = await createActor(
    nodeA,
    uniqueShort('actor-rebind-process'),
    SpotActorTransferNames.actorTypeStateful,
    201
  );
  const oldConnector = await connectAndBind(
    options.sessionAStreamEndpoint,
    scenario,
    actor,
    uniqueShort('bind-old-process')
  );
  let disconnectedAt: number | undefined;
  const disconnected = new Promise<number>((resolve) => {
    oldConnector.onDisconnected(() => {
      disconnectedAt = Date.now();
      resolve(disconnectedAt);
    });
  });
  const callback = oldConnector.waitFor<BoundPushNotify>(SpotActorTransferNames.packetBoundNotify)
    .where((message) => message.payload.scenario === scenario
      && message.payload.actorId === actor.actorId
      && message.payload.marker === 'actor-binding-replaced')
    .timeout(5000)
    .submit();
  const callbackStartedAt = Date.now();
  const newBindStartedAt = Date.now();
  const newConnector = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionBStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 15000,
    requestTimeoutMs: 10000
  });
  await newConnector.connect();
  await newConnector.request({
    scenario,
    actorId: actor.actorId,
    objectGeneration: actor.objectGeneration,
    meshName: actor.meshName,
    nodeRid: actor.nodeRid,
    transferId: uniqueShort('bind-new-process')
  }).packetName(SpotActorTransferNames.packetBindActor).submit();
  const newBindTerminalAt = Date.now();
  try {
    await callback;
    const callbackAt = Date.now();
    const closedAt = await disconnected;
    require(closedAt - callbackStartedAt >= 90, 'NODE-SESS-001 old process closed before the 100 ms timer.');
    require(closedAt - callbackStartedAt < 1000, 'NODE-SESS-001 old process did not close after callback.');
    require(newBindTerminalAt < closedAt, 'NODE-SESS-001 other session lane did not progress before old close.');
    await delay(20);
    require(disconnectedAt !== undefined, 'NODE-SESS-001 old session process did not disconnect.');
    console.log(JSON.stringify({
      status: 'NODE_SESS_001_PROCESS_PASS',
      processes: ['actor-a', 'session-a', 'session-b'],
      bindTerminalBeforeCallback: newBindTerminalAt <= callbackAt,
      callbackToCloseMs: closedAt - callbackAt,
      newBindDurationMs: newBindTerminalAt - newBindStartedAt,
      otherSessionProgressBeforeClose: newBindTerminalAt < closedAt
    }));
  } finally {
    await newConnector.close();
    await oldConnector.close();
  }
}
