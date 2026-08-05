// OBS-C4: Shutdown은 relocation 없이 closing callback과 Session close를 수행한다 시나리오를 검증한다.
import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import { options, require, session } from '../Support/scenario-support.js';
import { metric, metrics, retireForceStopped, startDrain, waitFor, waitForDrain } from '../Support/observability-support.js';

export async function runObsC4(): Promise<void> {
  const connector = zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: true, intervalMs: 100, timeoutMs: 5000 },
    reconnect: { enabled: false }
  });
  await connector.connect();
  await waitFor(async () => await metrics(session),
    (values) => values.some((value) => value.name === 'zlink.stream.connections.active' && value.value === 1),
    'OBS-C4 server did not observe the active STREAM session');
  await startDrain(session, 100);
  const status = await waitForDrain(session, retireForceStopped,
    'OBS-C4 Session did not force stop', 5000);
  require(status.result?.reason === 5,
    `OBS-C4 force reason was '${status.result?.reason}'.`);
  await waitFor(async () => connector.closeReason, (value) => value === 'ServerDrain',
    'OBS-C4 connector did not preserve server drain close reason');
  require(metric(await metrics(session), 'zlink.drain.forced').value >= 1,
    'OBS-C4 forced drain metric was not incremented.');
  await connector.close();
}
