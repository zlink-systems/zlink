const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ServiceDiscoveryRegistry,
  ServiceTopologyRegistry
} = require('../../packages/framework/dist/internal');

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
