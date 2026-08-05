// SM-D14: TLS Stream에서 auth·relay·push를 수행한다 시나리오를 검증한다.
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
  AuthReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';

export async function runSmD14(options: ClientOptions): Promise<void> {
  ensure(options.sessionATlsStreamEndpoint.length > 0, 'SM-D14 TLS stream endpoint is required.');

  const actorId = 'actor-sm-d14-tls';
  const tls = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionATlsStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    connectTimeoutMs: 5000,
    requestTimeoutMs: 5000,
    waitTimeoutMs: 10000
  });
  await tls.connect();
  try {
    const auth = await tls
      .request({
        actorId,
        displayName: 'stream tls',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-a', 'SM-D14 auth reply mismatch.');

    const pushed = tls.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === actorId)
      .timeout(10000)
      .submit();
    const reply = await tls
      .request({ value: 'tls-push' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const notify = await pushed;

    ensure(reply.actorId === actorId, 'SM-D14 TLS actor reply mismatch.');
    ensure(reply.nodeRid === 'play-a', 'SM-D14 TLS actor node mismatch.');
    ensure(notify.payload.actorId === actorId, 'SM-D14 TLS push actor mismatch.');
    ensure(notify.payload.value === 'tls-push', 'SM-D14 TLS push payload mismatch.');
  } finally {
    await tls.close();
  }

  console.log('scenario SM-D14 passed');
}
