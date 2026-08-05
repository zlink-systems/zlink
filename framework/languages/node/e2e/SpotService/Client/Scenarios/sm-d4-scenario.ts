// SM-D4: 한 Session에 여러 Actors를 bind한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPingReq,
  ActorPushNotify,
  ActorPushReq,
  EvidenceWaitReq,
  MultiBindRes,
  MultiBindReq
} from '../../Shared/messages';
import { SpotServiceNames } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD4(options: ClientOptions): Promise<void> {
  const firstActorId = 'actor-sm-d4-x';
  const secondActorId = 'actor-sm-d4-y';
  const client = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
  await client.connect();
  try {
    const bound = await client
      .request({
        firstActorId,
        secondActorId,
        nodeRid: 'play-a'
      } satisfies MultiBindReq)
      .packetName('MultiBindReq')
      .timeout(5000)
      .submit<MultiBindRes>();
    ensure(bound.boundCount === 2, 'SM-D4 expected two bound actors.');

    const x = await client
      .request({ value: 'to-x' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .metadata(SpotServiceNames.actorIdMetadata, firstActorId)
      .timeout(5000)
      .submit<ActorPingRes>();
    const y = await client
      .request({ value: 'to-y' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .metadata(SpotServiceNames.actorIdMetadata, secondActorId)
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(x.actorId === firstActorId && x.value === 'to-x', 'SM-D4 x relay mismatch.');
    ensure(y.actorId === secondActorId && y.value === 'to-y', 'SM-D4 y relay mismatch.');

    const xPushed = client.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === firstActorId)
      .timeout(10000)
      .submit();
    const xPushReply = await client
      .request({ value: 'push-x' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .metadata(SpotServiceNames.actorIdMetadata, firstActorId)
      .timeout(5000)
      .submit<ActorPingRes>();
    const xNotify = await xPushed;
    ensure(xPushReply.actorId === firstActorId, 'SM-D4 x push reply actor mismatch.');
    ensure(xNotify.payload.value === 'push-x', 'SM-D4 x push payload mismatch.');

    const yPushed = client.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === secondActorId)
      .timeout(10000)
      .submit();
    const yPushReply = await client
      .request({ value: 'push-y' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .metadata(SpotServiceNames.actorIdMetadata, secondActorId)
      .timeout(5000)
      .submit<ActorPingRes>();
    const yNotify = await yPushed;
    ensure(yPushReply.actorId === secondActorId, 'SM-D4 y push reply actor mismatch.');
    ensure(yNotify.payload.value === 'push-y', 'SM-D4 y push payload mismatch.');

    let actorIdLessRequestFailed = false;
    try {
      await client
        .request({ value: 'missing-actor-id' } satisfies ActorPingReq)
        .packetName('ActorPingReq')
        .timeout(2000)
        .submit();
    } catch {
      actorIdLessRequestFailed = true;
    }
    ensure(actorIdLessRequestFailed, 'SM-D4 expected actor-id-less request to fail with multiple bound actors.');
  } finally {
    await client.close();
  }
  await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: [`entry-disconnected|rid=play-a|actor=${firstActorId}`],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);

  console.log('scenario SM-D4 passed');
}
