const assert = require('node:assert/strict');
const net = require('node:net');
const test = require('node:test');
const { once } = require('node:events');

const framework = require('../../packages/framework/dist/internal');

test.afterEach(async () => {
  await new Promise(resolve => setTimeout(resolve, 0));
});

test('wildcard RouteMesh advertisement admits a loopback RID-fenced peer for node-direct and channel calls', async () => {
  const localEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const remotePort = await reservePort();
  const remoteBindEndpoint = `tcp://0.0.0.0:${remotePort}`;
  const remoteConnectEndpoint = `tcp://127.0.0.1:${remotePort}`;
  const target = createTargetRuntime(remoteBindEndpoint, { channelServer: true });
  const caller = createCallerRuntime(localEndpoint, builder => {
    builder.peerConnections().connect('node-b', remoteConnectEndpoint);
    builder.channel('mesh').client();
  });

  try {
    await target.runtime.start();
    await caller.runtime.start();
    await waitForRouteMeshPeerReady(caller.runtime, 'mesh', 'node-b');

    assert.deepEqual(
      await caller.client.requestToNode('mesh', 'node-b', typedPacket('RoutePing', { value: 'ping' }))
        .timeout(1000)
        .submit(),
      { value: 'pong' }
    );
    assert.deepEqual(
      await caller.client.requestToChannel('mesh', typedPacket('RoutePing', { value: 'ping' }))
        .timeout(1000)
        .submit(),
      { value: 'pong' }
    );
  } finally {
    await caller.runtime.stop();
    await target.runtime.stop();
  }
});

test('endpoint-only admitted RouteMesh peer is a node-direct target after handshake RID resolution', async () => {
  const localEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const remoteEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
  const target = createTargetRuntime(remoteEndpoint);
  const caller = createCallerRuntime(localEndpoint, builder => {
    builder.peerConnections().connect(remoteEndpoint);
  });

  try {
    await target.runtime.start();
    await caller.runtime.start();
    await waitForRouteMeshPeerReady(caller.runtime, 'mesh', 'node-b');

    assert.deepEqual(
      await caller.client.requestToNode('mesh', 'node-b', typedPacket('RoutePing', { value: 'ping' }))
        .timeout(1000)
        .submit(),
      { value: 'pong' }
    );
  } finally {
    await caller.runtime.stop();
    await target.runtime.stop();
  }
});

function createTargetRuntime(bind, options = {}) {
  class RoutePingHandler {
    async handle() {
      return { value: 'pong' };
    }
  }
  const node = {
    router: {
      routingId: 'node-b',
      bind,
      advertiseHost: options.advertiseHost
    },
    routeRequestHandlers: [{ packetName: 'RoutePing', handlerType: RoutePingHandler }]
  };
  if (options.channelServer) {
    node.meshChannels = {
      mesh: {
        server: true,
        requestHandlers: [{ packetName: 'RoutePing', handlerType: RoutePingHandler }]
      }
    };
  }
  const registration = framework.createFrameworkRegistration({ spotNodes: { mesh: node } });
  return {
    runtime: new framework.ZLinkFrameworkRuntimeHost({
      registration,
      providerResolver: { resolve: type => new type() }
    })
  };
}

function createCallerRuntime(bind, configure) {
  const options = framework.createFrameworkOptions(builder => {
    const mesh = builder.addRouteMesh('mesh').listen(bind).routingId('node-a');
    configure(mesh);
  });
  const registration = framework.createFrameworkRegistration(options);
  const runtime = new framework.ZLinkFrameworkRuntimeHost({ registration });
  return {
    runtime,
    client: new framework.DefaultZLinkRouteClient(
      registration,
      runtime.routeTransport,
      runtime.spotRouterChannelIdForMesh
    )
  };
}

function typedPacket(packetName, value) {
  const PacketType = { [packetName]: class {} }[packetName];
  return Object.assign(new PacketType(), value);
}

async function reservePort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const port = server.address().port;
  await new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
  return port;
}

async function waitForRouteMeshPeerReady(runtime, meshName, peerRid) {
  await waitUntil(() => {
    const status = runtime.routeMeshRuntime.snapshot(meshName);
    return status.peers.some(peer =>
      String(peer.nodeRid) === peerRid && peer.state === framework.ZLinkPeerState.Ready);
  });
}

async function waitUntil(predicate) {
  const deadline = Date.now() + 10_000;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.fail('Condition did not become true before the deadline.');
}
