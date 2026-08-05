// SM-B8: Exact ActorRef로 current incarnation을 destroy한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  AuthRes,
  AuthReq,
  DestroyActorRes,
  DestroyActorReq,
  EvidenceWaitReq,
  SnapshotRes,
  SnapshotReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmB8(options: ClientOptions): Promise<void> {
  const actorId = `actor-sm-b8-destroy-${Date.now()}`;
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
        displayName: 'destroy',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-a', 'SM-B8 auth reply mismatch.');

    const destroyed = await client
      .request({ actorId } satisfies DestroyActorReq)
      .packetName('DestroyActorReq')
      .timeout(5000)
      .submit<DestroyActorRes>();
    ensure(
      destroyed.actorId === actorId && destroyed.destroyed,
      'SM-B8 destroy reply mismatch.'
    );

    let snapshotFailed = false;
    for (let attempt = 0; attempt < 10; attempt += 1) {
      try {
        await client
          .request({ actorId } satisfies SnapshotReq)
          .packetName('SnapshotReq')
          .timeout(1000)
          .submit<SnapshotRes>();
      } catch {
        snapshotFailed = true;
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, 150));
    }
    ensure(snapshotFailed, 'SM-B8 expected request to destroyed actor to fail.');

    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`actor-destroyed|rid=play-a|actor=${actorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      evidence.some((line) => line.includes(`actor-destroyed|rid=play-a|actor=${actorId}`)),
      'SM-B8 expected actor destroy evidence.'
    );
    ensure(
      evidence.every((line) => !line.includes(`actor-destroy-failed|rid=play-a|actor=${actorId}`)),
      'SM-B8 actor destroy reported a failure.'
    );
  } finally {
    await client.close();
  }

  console.log('scenario SM-B8 passed');
}
