// SM-B1: 같은 node의 User Spot으로 Join한다 시나리오를 검증한다.
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

export async function runSmB1(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-b1-local-${Date.now()}`;
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
        displayName: 'local actor',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-a', 'SM-B1 auth reply mismatch.');

    const pingMsg = await client
      .request({ value: 'b1' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(pingMsg.actorId === actorId, 'SM-B1 actor reply mismatch.');
    ensure(pingMsg.nodeRid === 'play-a', 'SM-B1 local node mismatch.');

    const expectedEvidence = [
      `entry-created|rid=play-a|actor=${actorId}`,
      `entry-joined|rid=play-a|actor=${actorId}`
    ];
    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-B1 evidence mismatch.'
    );
  } finally {
    await client.close();
  }

  console.log('scenario SM-B1 passed');
}
