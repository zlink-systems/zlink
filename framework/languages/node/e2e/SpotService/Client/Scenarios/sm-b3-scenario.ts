// SM-B3: Typed Actor request payload를 보존한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  AuthRes,
  AuthReq,
  ComplexActorRes,
  ComplexActorReq,
  EvidenceWaitReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmB3(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-b3-complex-${Date.now()}`;
  const client = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
  await client.connect();
  try {
    await client
      .request({
        actorId,
        displayName: 'complex actor',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();

    const complex = await client
      .request({
        displayName: 'Ada Lovelace',
        level: 42,
        tags: ['alpha', 'beta', 'gamma'],
        attributes: {
          role: 'analyst',
          region: 'west'
        }
      } satisfies ComplexActorReq)
      .packetName('ComplexActorReq')
      .timeout(5000)
      .submit<ComplexActorRes>();

    ensure(complex.actorId === actorId, 'SM-B3 actor id mismatch.');
    ensure(
      complex.displayName === 'Ada Lovelace' && complex.level === 42,
      'SM-B3 scalar payload mismatch.'
    );
    ensure(complex.tags.join(',') === 'alpha,beta,gamma', 'SM-B3 tag payload mismatch.');
    ensure(
      complex.attributes.role === 'analyst' && complex.attributes.region === 'west',
      'SM-B3 attribute payload mismatch.'
    );

    const expectedEvidence = [`actor-complex|rid=play-a|actor=${actorId}`];
    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-B3 evidence mismatch.'
    );
  } finally {
    await client.close();
  }

  console.log('scenario SM-B3 passed');
}
