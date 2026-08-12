// Runs the Actor half of the canonical SM-E1 missing-handler scenario.
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
import type { ClientOptions } from './client-options';
import { postJson } from '../../../http-client';
import { ensure } from './scenario-assert';

export async function runActorMissingHandlerVariant(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-e1-missing-${Date.now()}`;
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
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-a', 'SM-E1 actor auth reply mismatch.');

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
    ensure(failed, 'SM-E1 expected missing actor handler request to fail.');

    const expectedEvidence = [
      'dispatch-error|surface=actor|kind=request|reason=no_handler|action=reply_error|packet=MissingActorReq'
    ];
    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-E1 actor evidence mismatch.'
    );
  } finally {
    await client.close();
  }

}
