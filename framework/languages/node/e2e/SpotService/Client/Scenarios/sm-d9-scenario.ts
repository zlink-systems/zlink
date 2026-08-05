// SM-D9: Public inbound observer가 Stream packet을 기록한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  ZlinkStreamMessageKind
} from '@zlink-systems/stream-connector';
import type { ZlinkStreamInboundObservation } from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPingReq,
  AuthRes,
  AuthReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';

export async function runSmD9(options: ClientOptions): Promise<void> {
  const observed: ZlinkStreamInboundObservation[] = [];
  const client = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
  client.observeInbound((observation) => {
    observed.push(observation);
  });

  await client.connect();
  try {
    const auth = await client
      .request({
        actorId: 'actor-sm-d9',
        displayName: 'observer',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === 'actor-sm-d9', 'SM-D9 auth reply actor mismatch.');

    const first = await client
      .request({ value: 'observer-1' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(first.value === 'observer-1', 'SM-D9 first pingMsg value mismatch.');

    const second = await client
      .request({ value: 'observer-2' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(second.value === 'observer-2', 'SM-D9 second pingMsg value mismatch.');

    await waitForObservedReplies(observed, 2);
  } finally {
    await client.close();
  }

  console.log('scenario SM-D9 passed');
}

async function waitForObservedReplies(
  observed: readonly ZlinkStreamInboundObservation[],
  expectedCount: number
): Promise<void> {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const replies = observed.filter((item) =>
      item.kind === ZlinkStreamMessageKind.Response
      && item.requestSeq !== undefined
      && item.payloadLength > 0
    ).length;
    if (replies >= expectedCount) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  const summary = observed.map((item) => `${item.kind}:${item.name}:${item.requestSeq?.toString() ?? '-'}`).join(',');
  throw new Error(`SM-D9 inbound observer did not observe stream replies. observed=${summary}`);
}
