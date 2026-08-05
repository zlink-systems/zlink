import {
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';

let connector: ZlinkStreamConnector | undefined;

async function connect(endpoint: string, reconnect = false): Promise<void> {
  connector = zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    reconnect: { enabled: reconnect, maxAttempts: 20, initialDelayMs: 25, maxDelayMs: 100 },
    requestTimeoutMs: 5000
  });
  await connector.connect();
}

async function request(packetName: string, payload: unknown, compress = false): Promise<unknown> {
  if (!connector) throw new Error('Browser connector is not connected.');
  let call = connector.request(payload, Object).packetName(packetName).timeout(5000);
  if (compress) call = call.compress();
  return await call.submit();
}

async function close(): Promise<void> {
  await connector?.close();
  connector = undefined;
}

function state(): unknown {
  return { connectionState: connector?.state, closeReason: connector?.closeReason };
}

declare global {
  interface Window {
    zlinkBrowserConnector: {
      connect: typeof connect;
      request: typeof request;
      close: typeof close;
      state: typeof state;
    };
  }
}

window.zlinkBrowserConnector = { connect, request, close, state };
