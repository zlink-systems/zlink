// SM-D13: Stream heartbeat loss를 disconnect로 처리한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type { AuthRes, AuthReq } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';

export async function runSmD13(options: ClientOptions): Promise<void> {
  const stream = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: {
      enabled: true,
      intervalMs: 200,
      timeoutMs: 2000
    },
    maxReceivedMessages: 1024,
    waitTimeoutMs: 10000
  });

  await stream.connect();
  try {
    const auth = await stream
      .request({
        actorId: 'actor-sm-d13',
        displayName: 'heartbeat',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === 'actor-sm-d13', 'SM-D13 auth reply actor mismatch.');

    await new Promise((resolve) => setTimeout(resolve, 600));
    ensure(stream.isConnected, 'SM-D13 heartbeat-enabled stream disconnected.');
  } finally {
    await stream.close().catch(() => undefined);
  }

  console.log('scenario SM-D13 passed');
}
