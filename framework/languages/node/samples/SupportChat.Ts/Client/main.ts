import * as connector from '@zlink-systems/stream-connector';
import { loadSampleConfig } from './Configuration/sample-config';
import { SupportChatClientScenario } from './supportchat-client-scenario';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import { runBrowserSample } from '../../browser-client-runtime';

async function main(): Promise<void> {
  const config = await loadSampleConfig();
  const clients = Array.from({ length: 9 }, (_, index) => createClient(config.sessionStreamEndpoint, `client-${index + 1}`));
  try {
    await new SupportChatClientScenario().run(
      clients[0], clients[1], clients[2], clients[3], clients[4], clients[5], clients[6], clients[7], clients[8]
    );
  } finally {
    await Promise.allSettled(clients.map((client) => client.close()));
  }
  console.log('supportchat=completed');
  console.log('PASS SupportChat.Ts');
}

function createClient(endpoint: string, label: string): ZlinkStreamConnector {
  const client = connector.zlinkStreamConnectorFactory.create({
    endpoint,
    codec: connector.zlinkStreamJsonCodec,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate,
    requestTimeoutMs: 5000,
    waitTimeoutMs: 10000,
    heartbeat: { enabled: false }
  });
  client.observeInbound((observation) => {
    console.log(`stream-inbound sample=SupportChat client=${label} kind=${observation.kind} name=${observation.name}`);
  });
  return client;
}

void runBrowserSample('SupportChat.Ts', main);
