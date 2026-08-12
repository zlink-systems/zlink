// Runs the local-owner half of the canonical SM-D2 Session relay scenario.
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
import type { ClientOptions } from './client-options';
import { postJson } from '../../../http-client';
import { ensure } from './scenario-assert';

export async function runLocalActorRelayVariant(options: ClientOptions): Promise<void> {
  await waitForControlRoute(options.sessionAUrl, 'play-a');

  const actorId = 'actor-sm-d2-local';
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
        displayName: 'local relay',
        nodeRid: 'play-a'
    } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === actorId && auth.nodeRid === 'play-a', 'SM-D2 local auth reply mismatch.');
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-a|actor=${actorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    const pushed = client.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === actorId)
      .timeout(10000)
      .submit();
    const reply = await client
      .request({ value: 'push-local' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const notify = await pushed;

    ensure(reply.actorId === actorId, 'SM-D2 local actor reply mismatch.');
    ensure(reply.nodeRid === 'play-a', 'SM-D2 local actor request reached the wrong node.');
    ensure(notify.payload.actorId === actorId, 'SM-D2 local push actor mismatch.');
    ensure(notify.payload.value === 'push-local', 'SM-D2 local push value mismatch.');
  } finally {
    await client.close();
  }

}

async function waitForControlRoute(sessionUrl: string, targetRid: string): Promise<void> {
  const deadline = Date.now() + 30000;
  let lastError: unknown;
  while (Date.now() < deadline) {
    try {
      const reply = await postJson<ControlPingRes>(sessionUrl, `/channel/control-pingMsg/${targetRid}`, {
        value: `sm-d2-local-${targetRid}-ready`
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
