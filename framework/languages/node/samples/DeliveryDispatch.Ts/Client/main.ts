import * as connector from '@zlink-systems/stream-connector';
import { loadSampleConfig } from './Configuration/sample-config';
import { DeliveryDispatchClientScenario } from './deliverydispatch-client-scenario';
import { SampleTimings } from '../Shared/Configuration/sample-names';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { BrowserHttpClientFactory, runBrowserSample } from '../../browser-client-runtime';

async function main(): Promise<void> {
  const config = await loadSampleConfig();
  const http = BrowserHttpClientFactory.create(config.dispatchApiHttpUrl)
    .timeout(SampleTimings.clientTimeout)
    .build();
  try {
    await new DeliveryDispatchClientScenario().run(
      http,
      () => createClient(config.sessionStreamEndpoint),
      () => createClient(config.courierStreamEndpoint),
      () => createClient(config.courierStreamEndpoint),
      AbortSignal.timeout(SampleTimings.clientTimeout)
    );
  } finally {
    await http.close();
  }
  console.log('deliverydispatch=completed');
  console.log('PASS DeliveryDispatch.Ts');
}

function createClient(sessionEndpoint: string): ZlinkStreamConnector {
  return connector.zlinkStreamConnectorFactory.create({
    endpoint: sessionEndpoint,
    codec: connector.zlinkStreamJsonCodec,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    requestTimeoutMs: SampleTimings.requestTimeout,
    waitTimeoutMs: SampleTimings.clientTimeout,
    heartbeat: { enabled: false }
  });
}

void runBrowserSample('DeliveryDispatch.Ts', main);

export {};
