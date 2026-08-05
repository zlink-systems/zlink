// OBS-A2: Dispatch 실패 record에도 flow를 남긴다 시나리오를 검증한다.
import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import { options, require, session } from '../Support/scenario-support.js';
import { readFlowLog, waitFor } from '../Support/observability-support.js';

export async function runObsA2(): Promise<void> {
  const connector = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    requestTimeoutMs: 3000
  });
  await connector.connect();
  let failed = false;
  try {
    await connector.request({ invalid: true }).packetName('UnknownObservabilityPacket').timeout(2000).submit();
  } catch {
    failed = true;
  } finally {
    await connector.close();
  }
  require(failed, 'OBS-A2 unknown packet unexpectedly succeeded.');
  const line = await waitFor(async () => (await readFlowLog(session))
    .split('\n').find((candidate) => candidate.includes('phase=error') && candidate.includes('flow=')) ?? '',
  (value) => value.length > 0, 'OBS-A2 dispatch error line did not contain flow');
  require(/flow=[0-9a-f-]{36}/.test(line), 'OBS-A2 error flow is not a UUID.');
}
