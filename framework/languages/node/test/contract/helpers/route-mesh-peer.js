const framework = require('../../../packages/framework/dist/internal');

class RoutePing {
  constructor(value) {
    this.value = value;
  }
}

class RouteNotice {
  constructor(value) {
    this.value = value;
  }
}

class RoutePingHandler {
  async handle(payload) {
    return { value: payload.value === 'ping' ? 'pong' : 'unexpected' };
  }
}

class RouteNoticeHandler {
  async handle(payload) {
    process.send?.({ type: 'notice', value: payload.value });
  }
}

const options = JSON.parse(process.argv[2]);
const node = {
  router: {
    routingId: 'node-b',
    bind: options.bind,
    manualPeerConnections: [{ peerRid: 'node-a', endpoint: options.peer }]
  },
  meshChannels: options.mode === 'client-channel'
    ? { mesh: { client: true, weight: 0 } }
    : undefined
};
if (options.mode === 'server-direct') {
  node.routeRequestHandlers = [{ packetName: 'RoutePing', handlerType: RoutePingHandler }];
  node.routeSendHandlers = [{ packetName: 'RouteNotice', handlerType: RouteNoticeHandler }];
}
const registration = framework.createFrameworkRegistration({
  spotNodes: { mesh: node }
});
const providers = new Map();
const runtime = new framework.ZLinkFrameworkRuntimeHost({
  registration,
  providerResolver: {
    resolve(type) {
      let provider = providers.get(type);
      if (provider === undefined) {
        provider = new type();
        providers.set(type, provider);
      }
      return provider;
    }
  }
});
const client = new framework.DefaultZLinkRouteClient(
  registration,
  runtime.routeTransport,
  runtime.spotRouterChannelIdForMesh
);

process.on('message', (message) => {
  if (message?.type === 'run') {
    void runClient(message.mode).then(
      result => process.send?.({ type: 'result', result }),
      error => process.send?.({
        type: 'error',
        message: error instanceof Error ? error.stack ?? error.message : String(error)
      })
    );
  }
  if (message?.type === 'stop') {
    void runtime.stop().finally(() => process.exit(0));
  }
});

void runtime.start().then(
  () => process.send?.({ type: 'ready' }),
  error => {
    process.send?.({
      type: 'error',
      message: error instanceof Error ? error.stack ?? error.message : String(error)
    });
    void runtime.stop().finally(() => process.exit(1));
  }
);

async function runClient(mode) {
  if (mode === 'direct') {
    await retryReachable(() =>
      client.requestToNode('mesh', 'node-a', new RoutePing('ready')).timeout(1000).submit()
    );
    await client.sendToNode('mesh', 'node-a', new RouteNotice('one-way')).submit();
    return retryReachable(() =>
      client.requestToNode('mesh', 'node-a', new RoutePing('ping')).timeout(1000).submit()
    );
  }
  return retryReachable(() =>
    client.requestToChannel('mesh', new RoutePing('ping')).timeout(1000).submit()
  );
}

async function retryReachable(submit) {
  const deadline = Date.now() + 5000;
  let lastError;
  while (Date.now() < deadline) {
    try {
      return await submit();
    } catch (error) {
      lastError = error;
      await new Promise(resolve => setImmediate(resolve));
    }
  }
  throw lastError;
}
