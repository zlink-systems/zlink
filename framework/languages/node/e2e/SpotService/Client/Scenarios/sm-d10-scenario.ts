// SM-D10: Stream backpressure를 Session별로 격리한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  ZlinkStreamErrorCode
} from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPingReq,
  ActorPushNotify,
  ActorPushReq,
  AuthReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';

export async function runSmD10(options: ClientOptions): Promise<void> {
  const congested = createStreamClient(options.sessionAStreamEndpoint, 1);
  const isolated = createStreamClient(options.sessionAStreamEndpoint, 1024);
  let congestedPushCount = 0;
  let droppedCount = 0;
  const congestedPushValues: string[] = [];
  const congestedErrorSubscription = congested.onErrorReceived((error) => {
    if (error.code === ZlinkStreamErrorCode.ReceivedMessageDropped) {
      droppedCount += 1;
    }
  });
  const congestedPushSubscription = congested.on<ActorPushNotify>('ActorPushNotify', async (message) => {
    congestedPushCount += 1;
    congestedPushValues.push(message.payload.value);
    await new Promise((resolve) => setTimeout(resolve, 150));
  });

  await congested.connect();
  await isolated.connect();
  try {
    await congested
      .request({
        actorId: 'actor-sm-d10-congested',
        displayName: 'stream backpressure',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit();
    await isolated
      .request({
        actorId: 'actor-sm-d10-isolated',
        displayName: 'stream backpressure peer',
        nodeRid: 'play-b'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit();

    for (let index = 0; index < 8; index += 1) {
      const reply = await congested
        .request({ value: `burst-${index}` } satisfies ActorPushReq)
        .packetName('ActorPushReq')
        .timeout(5000)
        .submit<ActorPingRes>();
      ensure(reply.actorId === 'actor-sm-d10-congested', 'SM-D10 congested reply actor mismatch.');
    }

    await new Promise((resolve) => setTimeout(resolve, 1200));
    ensure(droppedCount > 0, 'SM-D10 expected received-message drop notification.');
    ensure(congestedPushCount < 8, 'SM-D10 expected bounded received-message queue for congested session.');

    const stillAlive = await congested
      .request({ value: 'after-backpressure' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(stillAlive.actorId === 'actor-sm-d10-congested', 'SM-D10 congested session stopped routing.');
    ensure(stillAlive.value === 'after-backpressure', 'SM-D10 congested session reply mismatch.');
    ensure(congestedPushValues.every((value) => value.startsWith('burst-')), 'SM-D10 congested push value mismatch.');

    const isolatedPush = isolated.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === 'actor-sm-d10-isolated')
      .timeout(10000)
      .submit();
    const isolatedReply = await isolated
      .request({ value: 'isolated-push' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const isolatedNotify = await isolatedPush;
    ensure(isolatedReply.actorId === 'actor-sm-d10-isolated', 'SM-D10 isolated reply actor mismatch.');
    ensure(isolatedNotify.payload.actorId === 'actor-sm-d10-isolated', 'SM-D10 isolated push actor mismatch.');
    ensure(isolatedNotify.payload.value === 'isolated-push', 'SM-D10 isolated session push mismatch.');
  } finally {
    congestedErrorSubscription.dispose();
    congestedPushSubscription.dispose();
    await Promise.allSettled([congested.close(), isolated.close()]);
  }

  console.log('scenario SM-D10 passed');
}

function createStreamClient(endpoint: string, maxReceivedMessages: number) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    maxReceivedMessages,
    waitTimeoutMs: 10000
  });
}
