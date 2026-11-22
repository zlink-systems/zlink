const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkLocationRole } = require('../../packages/framework/dist/internal');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ZLinkChannelSocketRegistry
} = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  spotNodeAutoConnectCapability
} = require('../../packages/framework/dist/runtime/spots/spot-node-autoconnect');

function fixture() {
  const calls = [];
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    setReadableHandler() {},
    bind() {},
    close() {},
    localEndpoint: () => 'inproc://connect-intent-local',
    routesSnapshot: () => [],
    connect(endpoint) {
      calls.push(['connect', endpoint]);
    },
    connectToRoutingId(rid, endpoint) {
      calls.push(['connect', endpoint, rid]);
    },
    disconnect(endpoint) {
      calls.push(['disconnect', endpoint]);
    }
  };
  const node = new ZLinkNodeRawMeshBackend(
    'mesh',
    'local',
    {
      createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} })
    },
    new ApplicationJobQueue(resolveApplicationJobQueueConfiguration())
  );
  node.setBind('inproc://connect-intent-local');
  node.start();
  return { node, calls };
}

for (const expectedRid of [undefined, 'peer']) {
  test(`repeated mesh configuration retains one connect intent (${expectedRid ?? 'endpoint'})`, async () => {
    const { node, calls } = fixture();
    try {
      const options = { endpoint: 'inproc://peer', expectedRid };
      const intent = await node.connectPeer(options);
      assert.equal(await node.connectPeer(options), intent);
      assert.equal(calls.filter(([kind]) => kind === 'connect').length, 1);
      node.removePeerConnection(intent);
      assert.notEqual(await node.connectPeer(options), intent);
      assert.equal(calls.filter(([kind]) => kind === 'connect').length, 2);
    } finally {
      await node.close();
    }
  });
}

for (const replacement of [
  { expectedRid: 'replacement', expectedSecurityIdentity: 'a' },
  { expectedRid: 'peer', expectedSecurityIdentity: 'b' },
  { expectedRid: 'peer', expectedSecurityIdentity: 'a', expectedLifecycleGeneration: 19n },
  { expectedSecurityIdentity: 'a' }
]) {
  test(`mesh expectation changes retire the endpoint before reconnect (${String(replacement.expectedRid)}, ${replacement.expectedSecurityIdentity}, ${String(replacement.expectedLifecycleGeneration)})`, async () => {
    const { node, calls } = fixture();
    try {
      const endpoint = 'inproc://peer';
      const first = await node.connectPeer({
        endpoint,
        expectedRid: 'peer',
        expectedSecurityIdentity: 'a'
      });
      const second = await node.connectPeer({ endpoint, ...replacement });
      assert.notEqual(first, second);
      assert.deepEqual(calls, [
        ['connect', endpoint, 'peer'],
        ['disconnect', endpoint],
        replacement.expectedRid === undefined
          ? ['connect', endpoint]
          : ['connect', endpoint, replacement.expectedRid]
      ]);
      node.removePeerConnection(first);
      assert.equal(calls.length, 3, 'a retired handle cannot disconnect the replacement');
    } finally {
      await node.close();
    }
  });
}

test('removing an old intent unregisters its endpoint and preserves the replacement expectation', async () => {
  const { node, calls } = fixture();
  const endpoint = 'inproc://peer';
  try {
    const first = await node.connectPeer({
      endpoint,
      expectedRid: 'peer',
      expectedLifecycleGeneration: 1n
    });
    node.expectPeer({ endpoint, nodeRoutingId: 'peer', lifecycleGeneration: 2n });
    node.removePeerConnection(first);

    assert.deepEqual(calls, [
      ['connect', endpoint, 'peer'],
      ['disconnect', endpoint]
    ]);
    assert.equal(node.observePeerRoute('peer').expectation.lifecycleGeneration, 2n);
  } finally {
    await node.close();
  }
});

test('retiring an automatic intent leaves the admitted replacement at the same endpoint', async () => {
  const bind = 'tcp://127.0.0.1:5001';
  const target = {
    targetKey: 'shared:1',
    nodeRid: 'shared',
    lifecycleGeneration: 1n,
    endpoint: 'tcp://127.0.0.1:5002',
    role: ZLinkLocationRole.Router
  };
  const removedIntents = [];
  const disconnectedLifecycles = [];
  const { executor } = spotNodeAutoConnectCapability(
    'mesh',
    { router: { bind } },
    {
      status: () => ({ routingId: 'game', localEndpoint: bind }),
      connectPeer: async () => 1n,
      removePeerConnection: (id) => removedIntents.push(id),
      peers: () => [
        {
          routingId: target.nodeRid,
          endpoint: target.endpoint,
          lifecycleGeneration: 2n
        }
      ],
      disconnectPeer: (_rid, lifecycle) => disconnectedLifecycles.push(lifecycle)
    }
  );

  assert.equal(await executor.connect(target), true);
  executor.disconnect(target);

  assert.deepEqual(removedIntents, [1n]);
  assert.deepEqual(disconnectedLifecycles, []);
});

test('route manual configuration uses one connect per normalized endpoint', async () => {
  const calls = [];
  const router = {
    setChannelName() {},
    setRoutingId() {},
    dispose() {},
    connect(endpoint) {
      calls.push(endpoint);
    }
  };
  const registry = new ZLinkChannelSocketRegistry(
    {
      routeChannelOptions: new Map([
        [
          'route',
          { routerChannelId: 'route', manualConnections: ['inproc://peer', 'inproc://peer'] }
        ]
      ])
    },
    { createRouterSocket: () => router },
    {}
  );
  try {
    assert.equal(registry.routeMeshSocket('route'), router);
    assert.equal(registry.routeMeshSocket('route'), router);
    assert.deepEqual(calls, ['inproc://peer']);
  } finally {
    await registry.dispose();
  }
});
