// SM-B5: Handler 없는 Actor request를 관찰한다 시나리오를 검증한다.
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

export async function runSmB5(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-b5-missing-${Date.now()}`;
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
        displayName: 'missing handler actor',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-a', 'SM-B5 auth reply mismatch.');

    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-a|actor=${actorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    let failed = false;
    try {
      await client
        .request({ value: 'missing-handler' } satisfies ActorPingReq)
        .packetName('MissingActorReq')
        .timeout(2000)
        .submit<ActorPingRes>();
    } catch {
      failed = true;
    }
    ensure(failed, 'SM-B5 expected missing actor handler request to fail.');

    const expectedEvidence = [
      'dispatch-error|surface=actor|kind=request|reason=no_handler|action=reply_error|packet=MissingActorReq'
    ];
    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-B5 evidence mismatch.'
    );
  } finally {
    await client.close();
  }

  console.log('scenario SM-B5 passed');
}
