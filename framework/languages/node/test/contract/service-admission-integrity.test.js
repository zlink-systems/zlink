const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ServiceTopologyRegistry
} = require('../../packages/framework/dist/runtime/foundation/service-topology-registry');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

function descriptor(nodeRoutingId, notRequired) {
  return {
    meshName: 'admission-integrity',
    nodeRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: 2n,
    advertisedEndpoint: `inproc://${nodeRoutingId}`,
    channels: [],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: notRequired ? 'client' : 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 16,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

for (const notRequired of [false, true]) {
  const cases = [
    ['higher HELLO', 'hello', { descriptorRevision: 3n, placementWeight: 50 }, 'staleDescriptor'],
    ['higher ADMIT', 'admit', { descriptorRevision: 3n, placementWeight: 50 }, 'staleDescriptor'],
    ['higher UPDATE', 'update', { descriptorRevision: 3n, placementWeight: 50 }, undefined],
    ['identical HELLO', 'hello', {}, undefined],
    ['lower UPDATE', 'update', { descriptorRevision: 1n }, 'staleDescriptor'],
    ['conflicting revision', 'update', { placementWeight: 50 }, 'staleDescriptor'],
    [
      'immutable endpoint',
      'update',
      { descriptorRevision: 3n, advertisedEndpoint: 'inproc://changed' },
      'invalidDescriptor'
    ]
  ];
  for (const [name, command, change, rejected] of cases) {
    test(`admission integrity ${name} (notRequired=${notRequired})`, () => {
      const registry = new ServiceTopologyRegistry(descriptor('local', notRequired));
      const peer = descriptor('peer', notRequired);
      const accepted = notRequired ? 'notRequired' : 'admitted';
      assert.equal(
        registry.admit(peer, 'route', undefined, undefined, wire.M6aServiceWireCommand.hello),
        accepted
      );
      let livenessCalls = 0;
      const incoming = { ...peer, ...change };
      assert.equal(
        registry.admit(
          incoming,
          'route',
          undefined,
          () => {
            livenessCalls++;
          },
          wire.M6aServiceWireCommand[command]
        ),
        rejected ?? accepted
      );
      assert.deepEqual(registry.routeAdmission('peer').descriptor, rejected ? peer : incoming);
      if (rejected) assert.equal(livenessCalls, 0, 'rejected input cannot mutate liveness');
    });
  }
}

test('same revision compares the received descriptor bytes before accepting idempotence', () => {
  const registry = new ServiceTopologyRegistry(descriptor('local', false));
  const peer = descriptor('peer', false);
  const original = Buffer.from([1, 2]);
  assert.equal(
    registry.admit(peer, 'route', undefined, undefined, wire.M6aServiceWireCommand.hello, original),
    'admitted'
  );
  original.fill(0);
  assert.equal(
    registry.admit(
      peer,
      'route',
      undefined,
      undefined,
      wire.M6aServiceWireCommand.admit,
      Buffer.from([1, 2])
    ),
    'admitted'
  );
  assert.equal(
    registry.admit(
      peer,
      'route',
      undefined,
      undefined,
      wire.M6aServiceWireCommand.hello,
      Buffer.from([1, 3])
    ),
    'staleDescriptor'
  );
});
