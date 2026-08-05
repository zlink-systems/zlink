// SM-D15: Channel→Actor→bound Session push 사슬을 완료한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  AuthReq,
  AuthRes,
  CrossRoleActorPushReq,
  CrossRoleActorPushRes,
  EvidenceWaitReq,
  ActorPushNotify,
  ActorPingReq,
  ActorPingRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD15(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-d15-${Date.now()}`;
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
        actorId,
        displayName: actorId,
        nodeRid: 'play-a'
    } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.nodeRid.trim().length > 0, 'SM-D15 auth reply did not include actor node rid.');
    ensure(
      auth.generation !== undefined && BigInt(auth.generation) > 0n,
      'SM-D15 auth reply did not include a positive actor generation.'
    );
    const probe = await client
      .request({ value: 'd15-bind-probe' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(probe.actorId === actorId, 'SM-D15 bind probe actor mismatch.');

    const pushed = client.waitFor<ActorPushNotify>('ActorPushNotify')
      .timeout(10000)
      .submit();
    const reply = await postJson<CrossRoleActorPushRes>(options.playBUrl, '/actor/cross-role/push', {
      actorId,
      nodeRid: auth.nodeRid,
      generation: auth.generation,
      value: 'd15-cross-role'
    } satisfies CrossRoleActorPushReq);
    ensure(reply.delivered && reply.actorId === actorId, 'SM-D15 cross-role push reply mismatch.');
    const notify = await pushed;
    ensure(notify.payload.actorId === actorId, 'SM-D15 actor push notify actor mismatch.');
    ensure(notify.payload.value === 'd15-cross-role', 'SM-D15 actor push notify value mismatch.');
  } finally {
    await client.close();
  }

  const expected = [
    `cross-role-entry|rid=play-b|target=play-a|actor=${actorId}|value=d15-cross-role`,
    `cross-role-push|rid=play-a|actor=${actorId}|value=d15-cross-role`
  ];
  await postJson<string[]>(options.playBUrl, '/evidence/wait', {
    containsAll: [expected[0]],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: [expected[1]],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);

  console.log('scenario SM-D15 passed');
}
