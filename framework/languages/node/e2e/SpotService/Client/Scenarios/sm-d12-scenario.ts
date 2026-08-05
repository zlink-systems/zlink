// SM-D12: 다른 gateway reconnect 뒤 Actor state를 rebind한다 시나리오를 검증한다.
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
  AuthRes,
  AuthReq,
  EvidenceWaitReq,
  SnapshotRes,
  SnapshotReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD12(options: ClientOptions): Promise<void> {
  const actorId = 'actor-sm-d12-transfer';
  const first = createStreamClient(options.sessionAStreamEndpoint);
  let second: ReturnType<typeof createStreamClient> | undefined;

  await first.connect();
  try {
    await first
      .request({
        actorId,
        displayName: 'api transfer',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();

    const firstReply = await first
      .request({ value: 'before-transfer' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(firstReply.actorId === actorId, 'SM-D12 first api actor mismatch.');
    ensure(firstReply.nodeRid === 'play-a', 'SM-D12 first api node mismatch.');
    ensure(firstReply.seen === 1, 'SM-D12 expected initial actor state.');

    await first.close();
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-disconnected|rid=play-a|actor=${actorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    second = createStreamClient(options.sessionBStreamEndpoint);
    await second.connect();
    await second
      .request({
        actorId,
        displayName: 'api transfer',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();

    const snapshot = await second
      .request({ actorId } satisfies SnapshotReq)
      .packetName('SnapshotReq')
      .timeout(5000)
      .submit<SnapshotRes>();
    ensure(snapshot.actorId === actorId, 'SM-D12 snapshot actor mismatch.');
    ensure(snapshot.seen === 1, 'SM-D12 actor state was not preserved across apis.');

    const pushed = second.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === actorId)
      .timeout(10000)
      .submit();
    const resumed = await second
      .request({ value: 'after-transfer' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const notify = await pushed;
    ensure(resumed.actorId === actorId, 'SM-D12 resumed actor mismatch.');
    ensure(resumed.nodeRid === 'play-a', 'SM-D12 resumed node mismatch.');
    ensure(resumed.seen === 2, 'SM-D12 resumed actor state mismatch.');
    ensure(notify.payload.actorId === actorId, 'SM-D12 resumed push actor mismatch.');
    ensure(notify.payload.value === 'after-transfer', 'SM-D12 resumed push value mismatch.');
    ensure(notify.payload.seen === 2, 'SM-D12 resumed push state mismatch.');
  } finally {
    await first.close().catch(() => undefined);
    await second?.close().catch(() => undefined);
  }

  console.log('scenario SM-D12 passed');
}

function createStreamClient(endpoint: string) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    maxReceivedMessages: 1024,
    waitTimeoutMs: 10000
  });
}
