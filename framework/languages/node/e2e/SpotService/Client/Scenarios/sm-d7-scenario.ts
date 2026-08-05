// SM-D7: Stream auth 뒤 packet dispatch를 허용한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPingReq,
  AuthRes,
  AuthReq,
  EvidenceWaitReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD7(options: ClientOptions): Promise<void> {
  const client = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
  await client.connect();
  try {
    const auth = await client
      .request({
        actorId: 'actor-sm-d7',
        displayName: 'stream auth',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === 'actor-sm-d7', 'SM-D7 auth reply actor mismatch.');

    const reply = await client
      .request({ value: 'auth-ok' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(reply.actorId === 'actor-sm-d7', 'SM-D7 relay actor mismatch.');
    ensure(reply.value === 'auth-ok', 'SM-D7 relay value mismatch.');
  } finally {
    await client.close();
  }
  await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: ['entry-disconnected|rid=play-a|actor=actor-sm-d7'],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);

  console.log('scenario SM-D7 passed');
}
