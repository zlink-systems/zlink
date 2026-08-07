const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ServiceDiscoveryRegistry,
  ServiceTopologyRegistry
} = require('../../packages/framework/dist/internal');
const {
  SmoothWeightedSelection
} = require('../../packages/framework/dist/runtime/foundation/service-weighted-selection');

function serviceNode(nodeRoutingId, weight) {
  return {
    meshName: 'selection.mesh',
    nodeRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `tcp://${nodeRoutingId}`,
    channels: [{ name: 'orders', weight }],
    state: 'serving',
    securityIdentity: 'test',
    effectiveMaxMessageBytes: 1024,
    applicationVersion: 0n,
    protocolCapabilities: ['framework-service-v11'],
    objectRole: 'none',
    placementWeight: weight,
    activeCapacityLimit: 10,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

function clientServer(serverRoutingId, weight, state = 'serving', revision = 1n) {
  return {
    channelName: 'orders',
    serverRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: revision,
    weight,
    state,
    securityIdentity: 'test',
    effectiveMaxMessageBytes: 1024,
    advertisedEndpoint: `tcp://${serverRoutingId}`
  };
}

test('RouteMesh weighted selection uses smooth cumulative credit with RID tiebreak', () => {
  const topology = new ServiceTopologyRegistry(serviceNode('node-a', 5));
  assert.equal(topology.admit(serviceNode('node-b', 1), 'connection-b'), 'admitted');

  const selected = Array.from({ length: 12 }, () =>
    topology.selectChannel('orders').descriptor.nodeRoutingId
  );

  assert.deepEqual(selected, [
    'node-a', 'node-a', 'node-a', 'node-b',
    'node-a', 'node-a', 'node-a', 'node-a',
    'node-a', 'node-b', 'node-a', 'node-a'
  ]);
});

test('weighted selection keeps its cumulative state across descriptor updates', () => {
  const topology = new ServiceTopologyRegistry(serviceNode('node-a', 1));
  assert.equal(topology.admit(serviceNode('node-b', 1), 'connection-b'), 'admitted');

  assert.equal(topology.selectChannel('orders').descriptor.nodeRoutingId, 'node-a');
  topology.publishLocal({
    ...topology.localDescriptor(),
    descriptorRevision: 2n,
    activeCapacityUsed: 1
  });
  assert.equal(topology.selectChannel('orders').descriptor.nodeRoutingId, 'node-b');
});

test('cycle selection materializes cumulative state only when candidates rebuild', () => {
  const candidates = [
    { id: 'A', value: 'A', weight: 5 },
    { id: 'B', value: 'B', weight: 1 }
  ];
  const selection = new SmoothWeightedSelection(() => candidates);
  let clears = 0;
  const originalClear = selection.current.clear.bind(selection.current);
  selection.current.clear = () => {
    clears += 1;
    originalClear();
  };

  Array.from({ length: 12 }, () => selection.select());
  assert.equal(clears, 0);
  selection.rebuild();
  assert.equal(clears, 2);
});

test('ClientServer selection invalidates its cached eligible set on disconnect', () => {
  const discovery = new ServiceDiscoveryRegistry();
  assert.equal(discovery.admitClientServer(clientServer('server-a', 1), 'connection-a'), true);
  assert.equal(discovery.admitClientServer(clientServer('server-b', 1), 'connection-b'), true);

  // Populate the candidate cache before the state transition.
  assert.equal(discovery.selectClientServerConnection('orders')?.descriptor.serverRoutingId, 'server-a');
  assert.equal(discovery.markClientServerDisconnected('orders', 'server-b', 'connection-b'), true);

  assert.equal(
    discovery.selectClientServerConnection('orders')?.descriptor.serverRoutingId,
    'server-a'
  );
});

test('ClientServer weighted selection preserves credit across membership changes', () => {
  const discovery = new ServiceDiscoveryRegistry();
  assert.equal(discovery.admitClientServer(clientServer('A', 100), 'connection-a'), true);
  assert.equal(discovery.admitClientServer(clientServer('B', 300), 'connection-b'), true);

  assert.equal(discovery.selectClientServerConnection('orders').descriptor.serverRoutingId, 'B');
  assert.equal(discovery.admitClientServer(clientServer('C', 100), 'connection-c'), true);
  assert.equal(discovery.selectClientServerConnection('orders').descriptor.serverRoutingId, 'A');
});

test('weighted selection falls back without precomputing a cycle proportional to large weights', () => {
  const discovery = new ServiceDiscoveryRegistry();
  assert.equal(discovery.admitClientServer(clientServer('A', 4_999), 'connection-a'), true);
  assert.equal(discovery.admitClientServer(clientServer('B', 5_000), 'connection-b'), true);

  const selected = Array.from({ length: 8 }, () =>
    discovery.selectClientServerConnection('orders').descriptor.serverRoutingId);
  assert.deepEqual(selected, ['B', 'A', 'B', 'A', 'B', 'A', 'B', 'A']);
});
