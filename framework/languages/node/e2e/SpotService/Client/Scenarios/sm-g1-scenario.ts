// SM-G1: Play node crash 뒤 Application이 Actor를 recreate·rebind한다 시나리오를 검증한다.
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
  ControlPingReq,
  ControlPingRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson, postStatus } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmG1(options: ClientOptions): Promise<void> {
  await waitForControlRoute(options.sessionBUrl, 'play-b');

  const playA = createStreamClient(options.sessionAStreamEndpoint);
  const playB = createStreamClient(options.sessionBStreamEndpoint);
  let recovered: ReturnType<typeof createStreamClient> | undefined;

  await playA.connect();
  await playB.connect();
  try {
    await playA
      .request({
        actorId: 'actor-sm-g1-crash',
        displayName: 'crash-owner',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    await playB
      .request({
        actorId: 'actor-sm-g1-survivor',
        displayName: 'survivor',
        nodeRid: 'play-b'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();

    const beforeCrash = await playA
      .request({ value: 'before-crash' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(beforeCrash.nodeRid === 'play-a', 'SM-G1 play-a actor setup mismatch.');

    const beforeSurvivor = await playB
      .request({ value: 'before-crash' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(beforeSurvivor.nodeRid === 'play-b', 'SM-G1 play-b actor setup mismatch.');

    const crashStatus = await postStatus(`${options.playAUrl}/crash`);
    ensure(crashStatus >= 200 && crashStatus < 300, `SM-G1 crash endpoint returned HTTP ${crashStatus}.`);
    await new Promise((resolve) => setTimeout(resolve, 200));

    const afterCrashFailed = await fails(async () => {
      await playA
        .request({ value: 'after-crash' } satisfies ActorPingReq)
        .packetName('ActorPingReq')
        .timeout(1000)
        .submit();
    });
    ensure(afterCrashFailed, 'SM-G1 expected play-a actor request to fail after crash.');

    const survivor = await playB
      .request({ value: 'after-crash' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(survivor.actorId === 'actor-sm-g1-survivor', 'SM-G1 survivor actor mismatch.');
    ensure(survivor.nodeRid === 'play-b', 'SM-G1 survivor node mismatch.');

    recovered = createStreamClient(options.sessionBStreamEndpoint);
    await recovered.connect();
    await recovered
      .request({
        actorId: 'actor-sm-g1-crash',
        displayName: 'recovered-on-play-b',
        nodeRid: 'play-b'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    const rebound = await recovered
      .request({ value: 'rebound' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(rebound.actorId === 'actor-sm-g1-crash', 'SM-G1 rebound actor mismatch.');
    ensure(rebound.nodeRid === 'play-b', 'SM-G1 rebound node mismatch.');
  } finally {
    await playA.close().catch(() => undefined);
    await playB.close().catch(() => undefined);
    await recovered?.close().catch(() => undefined);
  }

  console.log('scenario SM-G1 passed');
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

async function fails(operation: () => Promise<void>): Promise<boolean> {
  try {
    await operation();
    return false;
  } catch {
    return true;
  }
}

async function waitForControlRoute(sessionUrl: string, targetRid: string): Promise<void> {
  const deadline = Date.now() + 30000;
  let lastError: unknown;
  while (Date.now() < deadline) {
    try {
      const reply = await postJson<ControlPingRes>(sessionUrl, `/channel/control-pingMsg/${targetRid}`, {
        value: `sm-g1-${targetRid}-ready`
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
