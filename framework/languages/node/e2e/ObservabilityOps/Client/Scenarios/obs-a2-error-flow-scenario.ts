// OBS-A2: Dispatch 실패 record에도 flow를 남긴다 시나리오를 검증한다.
import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import { options, require, session } from '../Support/scenario-support.js';
import { readFlowRecords, waitFor } from '../Support/observability-support.js';

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
  const record = await waitFor(async () => (await readFlowRecords(session))
    .find((candidate) => candidate.eventId === 'zlink.dispatch_error'),
  (value) => value !== undefined, 'OBS-A2 dispatch error record did not contain flow');
  require(typeof record?.flow_id === 'string' && /^[0-9a-f-]{36}$/.test(record.flow_id),
    'OBS-A2 error flow is not a UUID.');
}
