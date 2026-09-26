const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

// Each RouteMesh binds an OS-assigned port; peers connect to the endpoint it
// bound, read from its node status after start.
const ANY_LOOPBACK_PORT = 'tcp://127.0.0.1:*';

test.afterEach(async () => {
  await new Promise(resolve => setTimeout(resolve, 0));
});

test('wildcard RouteMesh advertisement admits a loopback RID-fenced peer for node-direct and channel calls', async () => {
  const target = createTargetRuntime('tcp://0.0.0.0:*', { channelServer: true });
  let caller;

  try {
    await target.runtime.start();
    // A wildcard bind advertises the same-family loopback address.
    const remoteConnectEndpoint = boundEndpoint(target.runtime);
    assert.match(remoteConnectEndpoint, /^tcp:\/\/127\.0\.0\.1:\d+$/);
    caller = createCallerRuntime(ANY_LOOPBACK_PORT, builder => {
      builder.peerConnections().connect('node-b', remoteConnectEndpoint);
      builder.channel('mesh').client();
    });
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
    await caller?.runtime.stop();
    await target.runtime.stop();
  }
});

test('endpoint-only admitted RouteMesh peer is a node-direct target after handshake RID resolution', async () => {
  const target = createTargetRuntime(ANY_LOOPBACK_PORT);
  let caller;

  try {
    await target.runtime.start();
    const remoteEndpoint = boundEndpoint(target.runtime);
    caller = createCallerRuntime(ANY_LOOPBACK_PORT, builder => {
      builder.peerConnections().connect(remoteEndpoint);
    });
    await caller.runtime.start();
    await waitForRouteMeshPeerReady(caller.runtime, 'mesh', 'node-b');

    assert.deepEqual(
      await caller.client.requestToNode('mesh', 'node-b', typedPacket('RoutePing', { value: 'ping' }))
        .timeout(1000)
        .submit(),
      { value: 'pong' }
    );
  } finally {
    await caller?.runtime.stop();
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

/** The endpoint a started runtime's RouteMesh node bound and advertises. */
function boundEndpoint(runtime) {
  return runtime.requirePrimaryMeshNode().status().localEndpoint;
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
