// SM-D2: Remote Actor를 Session에 bind하고 relay한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPushNotify,
  ActorPushReq,
  AuthRes,
  AuthReq,
  ControlPingRes,
  ControlPingReq,
  EvidenceWaitReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD2(options: ClientOptions): Promise<void> {
  await waitForControlRoute(options.sessionAUrl, 'play-b');

  const actorId = 'actor-sm-d2';
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
        displayName: 'remote relay',
        nodeRid: 'play-b'
    } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-b', 'SM-D2 auth reply mismatch.');
    await postJson<string[]>(options.playBUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-b|actor=${actorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    const pushed = client.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === actorId)
      .timeout(10000)
      .submit();
    const reply = await client
      .request({ value: 'push-remote' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const notify = await pushed;

    ensure(reply.actorId === actorId, 'SM-D2 actor reply mismatch.');
    ensure(reply.nodeRid === 'play-b', 'SM-D2 remote node mismatch.');
    ensure(notify.payload.actorId === actorId, 'SM-D2 push actor mismatch.');
    ensure(notify.payload.value === 'push-remote', 'SM-D2 push value mismatch.');
  } finally {
    await client.close();
  }

  console.log('scenario SM-D2 passed');
}

async function waitForControlRoute(sessionUrl: string, targetRid: string): Promise<void> {
  const deadline = Date.now() + 30000;
  let lastError: unknown;
  while (Date.now() < deadline) {
    try {
      const reply = await postJson<ControlPingRes>(sessionUrl, `/channel/control-pingMsg/${targetRid}`, {
        value: `sm-d2-${targetRid}-ready`
      } satisfies ControlPingReq);
      if (reply.nodeRid === targetRid) {
        return;
      }
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 500));
  }
  throw new Error(
    lastError instanceof Error
      ? `Control route did not become ready: ${targetRid}. Last error: ${lastError.message}`
      : `Control route did not become ready: ${targetRid}.`
  );
}
