// SM-D8: Stream reconnect는 새 auth·bind를 요구한다 시나리오를 검증한다.
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
  EvidenceWaitReq,
  SlowActorPingReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD8(options: ClientOptions): Promise<void> {
  const actorId = 'actor-sm-d8-reconnect';
  const first = createStreamClient(options.sessionAStreamEndpoint);
  let second: ReturnType<typeof createStreamClient> | undefined;

  await first.connect();
  try {
    await first
      .request({
        actorId,
        displayName: 'stream reconnect',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();

    const pending = first
      .request({
        value: 'before-disconnect',
        delayMs: 1000
      } satisfies SlowActorPingReq)
      .packetName('SlowActorPingReq')
      .timeout(10000)
      .submit();
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [
        `actor-slow-ping-start|rid=play-a|actor=${actorId}|spot=play-a|value=before-disconnect`
      ],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    await first.close();

    let pendingFailed = false;
    try {
      await pending;
    } catch {
      pendingFailed = true;
    }
    ensure(pendingFailed, 'SM-D8 expected pending request to fail after stream disconnect.');
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [
        `actor-slow-pingMsg|rid=play-a|actor=${actorId}|spot=play-a|value=before-disconnect`
      ],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    await postJson(options.sessionAUrl, '/shutdown', {});
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-disconnected|rid=play-a|actor=${actorId}`],
      timeoutMilliseconds: 30000
    } satisfies EvidenceWaitReq);

    second = createStreamClient(options.sessionBStreamEndpoint);
    await second.connect();
    await second
      .request({
        actorId,
        displayName: 'stream reconnect',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();

    const resumed = await second
      .request({ value: 'after-reconnect' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(resumed.actorId === actorId, 'SM-D8 reconnected actor mismatch.');
    ensure(resumed.nodeRid === 'play-a', 'SM-D8 reconnected node mismatch.');
    ensure(resumed.value === 'after-reconnect', 'SM-D8 reconnected value mismatch.');
  } finally {
    await first.close().catch(() => undefined);
    await second?.close().catch(() => undefined);
  }

  console.log('scenario SM-D8 passed');
}

function createStreamClient(endpoint: string) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
}
