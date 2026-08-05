// SM-D11: Stream과 Channel requests를 같은 client에서 분리한다 시나리오를 검증한다.
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
  ControlPingRes,
  ControlPingReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD11(options: ClientOptions): Promise<void> {
  const stream = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });

  await stream.connect();
  try {
    const auth = await stream
      .request({
        actorId: 'actor-sm-d11',
        displayName: 'stream and channel',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    ensure(auth.actorId === 'actor-sm-d11', 'SM-D11 auth reply actor mismatch.');

    const streamReply = await stream
      .request({ value: 'stream-side' } satisfies ActorPingReq)
      .packetName('ActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    ensure(streamReply.actorId === 'actor-sm-d11', 'SM-D11 stream request actor mismatch.');
    ensure(streamReply.value === 'stream-side', 'SM-D11 stream reply value mismatch.');
  } finally {
    await stream.close();
  }

  const channelReply = await postJson<ControlPingRes>(options.sessionAUrl, '/channel/control-pingMsg/play-a', {
    value: 'channel-side'
  } satisfies ControlPingReq);
  ensure(channelReply.nodeRid === 'play-a', 'SM-D11 channel request node mismatch.');
  ensure(channelReply.value === 'channel-side', 'SM-D11 channel reply value mismatch.');

  console.log('scenario SM-D11 passed');
}
