// OBS-B1: Stream connection과 reconnect metric을 확인한다 시나리오를 검증한다.
import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec
} from '@zlink-systems/stream-connector';
import { options, post, require, session } from '../Support/scenario-support.js';
import { metric, metrics, waitFor } from '../Support/observability-support.js';
import { MetricEvidenceCollector } from '../../Server/Support/metric-evidence-collector.js';

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

  const connectorMetrics = new MetricEvidenceCollector();
  const reconnecting = connector(connectorMetrics);
  await reconnecting.connect();
  await post(session, '/shutdown', {});
  await waitFor(async () => connectorMetrics.snapshot(),
    (values) => metricValue(values, 'zlink.stream.reconnects') >= 1,
    'OBS-B1 connector did not own reconnect attempt counting');
  await reconnecting.close();
}

function connector(meter?: MetricEvidenceCollector) {
  return zlinkStreamConnectorFactory.create({
    endpoint: options.sessionAStreamEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: true, intervalMs: 250, timeoutMs: 5000 },
    reconnect: { enabled: meter !== undefined, maxAttempts: 3, initialDelayMs: 20, maxDelayMs: 20 },
    meterProvider: meter?.provider
  });
}

function metricValue(values: readonly { name: string; value: number }[], name: string): number {
  return values.filter((value) => value.name === name).reduce((sum, value) => sum + value.value, 0);
}
