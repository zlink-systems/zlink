// OBS-B1: Stream connection과 reconnect metric을 확인한다 시나리오를 검증한다.
// Reconnect 계기는 client connector가 가질 것이 아니라는 결정으로 스펙에서
// 걷어냈으므로(stream-connector 스펙 32), 이 시나리오는 서버가 소유하는
// connection 계기만 확인한다.
import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import { options, require, session } from '../Support/scenario-support.js';
import { metric, metrics, waitFor } from '../Support/observability-support.js';

export async function runObsB1(): Promise<void> {
  const connectors = Array.from({ length: 3 }, () => connector());
  await Promise.all(connectors.map((value) => value.connect()));
  const active = await waitFor(async () => await metrics(session),
    (values) => metricValue(values, 'zlink.stream.connections.active') === 3,
    'OBS-B1 active connection gauge did not reach three');
  require(metric(active, 'zlink.stream.connections.active').kind === 'updown', 'OBS-B1 active metric kind mismatch.');
  await Promise.all(connectors.map((value) => value.close()));
  await waitFor(async () => await metrics(session),
    (values) => metricValue(values, 'zlink.stream.connections.active') === 0
      && metricValue(values, 'zlink.stream.connections.closed') >= 3,
    'OBS-B1 close metrics did not reflect the three closed sessions');
}

function connector() {
  return zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: true, intervalMs: 250, timeoutMs: 5000 },
    reconnect: { enabled: false }
  });
}

function metricValue(values: readonly { name: string; value: number }[], name: string): number {
  return values.filter((value) => value.name === name).reduce((sum, value) => sum + value.value, 0);
}
