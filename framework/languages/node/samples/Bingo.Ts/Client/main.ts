import * as connector from '@zlink-systems/stream-connector';
import { bingoProtobuf } from '../Shared/Contracts/protobuf-browser-codec';
import { BingoClientScenario } from './bingo-client-scenario';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { SampleTimings } from './Configuration/sample-names';
import { loadSampleConfig } from './Configuration/sample-config';
import { runBrowserSample } from '../../browser-client-runtime';
async function main(): Promise<void> {
  const config = await loadSampleConfig();
  const observedClients = new Set<string>();

  const client1 = createClient(config.sessionAEndpoint, 'player-1', observedClients);
  const client2 = createClient(config.sessionBEndpoint, 'player-2', observedClients);
  const observer = createClient(config.sessionBEndpoint, 'observer', observedClients);
  try {
    await new BingoClientScenario().run(client1, client2, observer);
    assertInboundObserved(observedClients, 'player-1');
    assertInboundObserved(observedClients, 'player-2');
    assertInboundObserved(observedClients, 'observer');
  } finally {
    await Promise.allSettled([
      client1.close(),
      client2.close(),
      observer.close()
    ]);
  }

  console.log('bingo=completed');
  console.log('PASS Bingo.Ts');
}

function createClient(
  sessionEndpoint: string,
  clientName: string,
  observedClients: Set<string>
): ZlinkStreamConnector {
  const client = connector.zlinkStreamConnectorFactory.create({
    endpoint: sessionEndpoint,
    codec: bingoProtobuf,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    waitTimeoutMs: SampleTimings.requestTimeout,
    heartbeat: { enabled: false }
  });
  client.observeInbound((observation) => {
    if (observation.name.length > 0 && Number.isInteger(observation.kind) && observation.payloadLength >= 0) {
      observedClients.add(clientName);
    }
    console.log(
      `stream-inbound sample=Bingo client=${clientName} kind=${observation.kind} ` +
      `name=${observation.name} seq=${observation.requestSeq?.toString() ?? '-'} ` +
      `flow=${observation.flowId ?? '-'} bytes=${observation.payloadLength}`
    );
  });
  return client;
}

function assertInboundObserved(observedClients: ReadonlySet<string>, clientName: string): void {
  if (!observedClients.has(clientName)) {
    throw new Error(`Bingo inbound observation missing for '${clientName}'.`);
  }
}

void runBrowserSample('Bingo.Ts', main);
