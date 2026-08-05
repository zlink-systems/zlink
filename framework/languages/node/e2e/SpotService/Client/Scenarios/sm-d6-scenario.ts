// SM-D6: Push는 current bound Session만 받는다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPushNotify,
  ActorPushReq,
  AuthRes,
  AuthReq,
  EvidenceWaitReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD6(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const boundActorId = `actor-sm-d6-bound-${suffix}`;
  const shadowActorId = `actor-sm-d6-shadow-${suffix}`;
  const bound = createStreamClient(options.sessionAStreamEndpoint);
  const unbound = createStreamClient(options.sessionBStreamEndpoint);
  let unboundTargetPushCount = 0;
  const unboundSubscription = unbound.on<ActorPushNotify>('ActorPushNotify', (message) => {
    if (message.payload.actorId === boundActorId) {
      unboundTargetPushCount += 1;
    }
  });

  await bound.connect();
  await unbound.connect();
  try {
    const shadowAuth = await unbound
      .request({
        actorId: shadowActorId,
        displayName: 'unbound',
        nodeRid: 'play-b'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(
      shadowAuth.actorId === shadowActorId && shadowAuth.nodeRid === 'play-b',
      'SM-D6 shadow auth reply mismatch.'
    );
    await postJson<string[]>(options.playBUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-b|actor=${shadowActorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    const boundAuth = await bound
      .request({
        actorId: boundActorId,
        displayName: 'bound',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(
      boundAuth.actorId === boundActorId && boundAuth.nodeRid === 'play-a',
      'SM-D6 bound auth reply mismatch.'
    );
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-a|actor=${boundActorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    const pushed = bound.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === boundActorId)
      .timeout(10000)
      .submit();
    const reply = await bound
      .request({ value: 'push-bound-only' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const notify = await pushed;
    ensure(reply.actorId === boundActorId, 'SM-D6 push reply actor mismatch.');
    ensure(reply.nodeRid === 'play-a', 'SM-D6 push reply node mismatch.');
    ensure(notify.payload.actorId === boundActorId, 'SM-D6 push actor mismatch.');
    ensure(notify.payload.value === 'push-bound-only', 'SM-D6 push value mismatch.');

    await new Promise((resolve) => setTimeout(resolve, 200));
    ensure(unboundTargetPushCount === 0, 'SM-D6 unbound session received target actor push.');
  } finally {
    unboundSubscription.dispose();
    await Promise.allSettled([bound.close(), unbound.close()]);
  }

  console.log('scenario SM-D6 passed');
}

function createStreamClient(endpoint: string) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
}
