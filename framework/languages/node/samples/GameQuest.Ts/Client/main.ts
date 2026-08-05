import * as connector from '@zlink-systems/stream-connector';
import { SampleNames } from '../Shared/Configuration/sample-names';
import { GameQuestClientScenario } from './gamequest-client-scenario';
import { loadSampleConfig } from './Configuration/sample-config';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { BrowserHttpClientFactory, runBrowserSample } from '../../browser-client-runtime';

async function main(): Promise<void> {
  const config = await loadSampleConfig();
  const apiA = BrowserHttpClientFactory.create(config.apiAHttpUrl).timeout(SampleNames.clientTimeout).build();
  const apiB = BrowserHttpClientFactory.create(config.apiBHttpUrl).timeout(SampleNames.clientTimeout).build();
  const missionA = BrowserHttpClientFactory.create(config.missionAHttpUrl).timeout(SampleNames.clientTimeout).build();
  const missionB = BrowserHttpClientFactory.create(config.missionBHttpUrl).timeout(SampleNames.clientTimeout).build();
  const apiAStream = createClient(config.apiAStreamEndpoint, 'api-a');
  const apiBStream = createClient(config.apiBStreamEndpoint, 'api-b');
  const apiBReconnectStream = createClient(config.apiBStreamEndpoint, 'api-b-reconnect');
  try {
    await new GameQuestClientScenario().run(
      apiA,
      apiB,
      missionA,
      missionB,
      apiAStream,
      apiBStream,
      apiBReconnectStream
    );
  } finally {
    await Promise.allSettled([
      apiAStream.close(),
      apiBStream.close(),
      apiBReconnectStream.close(),
      apiA.close(),
      apiB.close(),
      missionA.close(),
      missionB.close()
    ]);
  }
  console.log('gamequest=completed');
  console.log('PASS GameQuest.Ts');
}

function createClient(endpoint: string, name: string): ZlinkStreamConnector {
  const client = connector.zlinkStreamConnectorFactory.create({
    endpoint,
    codec: connector.zlinkStreamJsonCodec,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    requestTimeoutMs: SampleNames.requestTimeout,
    waitTimeoutMs: SampleNames.clientTimeout,
    heartbeat: { enabled: false }
  });
  client.observeInbound((observation) => {
    console.log(
      `stream-inbound sample=GameQuest client=${name} kind=${observation.kind} ` +
      `name=${observation.name} seq=${observation.requestSeq?.toString() ?? '-'} ` +
      `bytes=${observation.payloadLength}`
    );
  });
  client.onErrorReceived((error) => {
    console.error(`stream-error sample=GameQuest client=${name} code=${error.code} message=${error.message}`);
  });
  return client;
}

void runBrowserSample('GameQuest.Ts', main);

export {};
