import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';

let client: ZlinkStreamConnector | undefined;

async function connect(endpoint: string): Promise<void> {
  client = zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    reconnect: { enabled: true, maxAttempts: 20, initialDelayMs: 25, maxDelayMs: 100 }
  });
  await client.connect();
}

async function request(value: string): Promise<unknown> {
  if (!client) throw new Error('Connector has not been created.');
  const push = client
    .waitFor<{ value: string }>('EchoPush')
    .where((message) => message.payload.value === value)
    .submit();
  const call = client.request({ value }, Object).packetName('EchoReq');
  const reply = await call.submit<{ value: string }>();
  await push;
  return reply;
}

async function close(): Promise<void> {
  await client?.close();
  client = undefined;
}

function state(): unknown {
  return {
    connectionState: client?.state,
    closeReason: client?.closeReason
  };
}

declare global {
  interface Window {
    browserConnectorTest: {
      connect: typeof connect;
      request: typeof request;
      close: typeof close;
      state: typeof state;
    };
  }
}

window.browserConnectorTest = { connect, request, close, state };
