// SM-B4: Remote Actor request를 current owner로 보낸다 시나리오를 검증한다.
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

export async function runSmB4(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-b4-remote-${Date.now()}`;
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
        displayName: 'remote actor request',
        nodeRid: 'play-b'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-b', 'SM-B4 auth reply mismatch.');

    await postJson<string[]>(options.playBUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-b|actor=${actorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    const reply = await client
      .request({ value: 'sm-b4' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(reply.actorId === actorId, 'SM-B4 actor reply mismatch.');
    ensure(reply.nodeRid === 'play-b', 'SM-B4 remote actor request reached the wrong node.');

    const expectedEvidence = [`actor-pingMsg|rid=play-b|actor=${actorId}`];
    const evidence = await postJson<string[]>(options.playBUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-B4 evidence did not include remote actor request.'
    );
  } finally {
    await client.close();
  }
  await postJson<string[]>(options.playBUrl, '/evidence/wait', {
    containsAll: [`entry-disconnected|rid=play-b|actor=${actorId}`],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);

  console.log('scenario SM-B4 passed');
}
