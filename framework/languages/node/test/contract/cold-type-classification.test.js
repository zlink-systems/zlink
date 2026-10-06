const assert = require('node:assert/strict');
const test = require('node:test');
const { SubmitResult } = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

function descriptor(rid, type, overrides = {}) {
  return {
    meshName: 'play',
    nodeRoutingId: rid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `tcp://${rid}`,
    channels: [],
    state: 'serving',
    securityIdentity: 'test',
    applicationVersion: 0n,
    protocolCapabilities: [
      SERVICE_WIRE_REQUIRED_CAPABILITY,
      ...type.flatMap((value) => [`object-type:${value}`, `instance-spot-type:${value}`])
    ].sort(),
    objectRole: 'server',
    placementWeight: 1,
    activeCapacityLimit: 10,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0,
    ...overrides
  };
}

function fixture(targets) {
  class Lookup {
    constructor() {
      this.value = 1;
    }
  }
  const topology = new internal.ServiceTopologyRegistry(
    descriptor('source', [], { objectRole: 'client' })
  );
  for (const target of targets)
    assert.equal(topology.admit(target, `connection-${target.nodeRoutingId}`), 'admitted');
  const backend = new ZLinkNodeRawMeshBackend('play', 'source', {});
  backend.runtime = { topology, isPeerRouteReady: () => true };
  let submissions = 0;
  let selectedType;
  backend.sendToMissingInstanceSpot = async (target) => {
    submissions++;
    selectedType = target.stableType;
    return SubmitResult.Ok;
  };
  const transport = new internal.ZLinkHostSpotAddressTransport({
    resolver: () => ({ resolve: async () => undefined }),
    routed: {},
    meshNames: () => ['play'],
    meshNode: () => backend,
    defaultRequestTimeoutMs: 1000
  });
  return {
    request: (type) =>
      transport.requestToSpotAddress('room', new Lookup(), {
        instanceSpot: true,
        timeoutMs: 1000,
        ...(type === undefined ? {} : { instanceSpotType: type })
      }),
    send: (type) =>
      transport.sendToSpotAddress('room', new Lookup(), {
        instanceSpot: true,
        ...(type === undefined ? {} : { instanceSpotType: type })
      }),
    submissions: () => submissions,
    selectedType: () => selectedType
  };
}

for (const [name, targets, type, kind] of [
  ['absent explicit type', [descriptor('target', ['U'])], 'T', 'NotFound'],
  ['no implicit type', [descriptor('target', [])], undefined, 'NotFound'],
  [
    'aggregate capacity exhausted',
    [descriptor('target', ['T'], { activeCapacityUsed: 10 })],
    'T',
    'Unavailable'
  ],
  [
    'pending capacity exhausted',
    [descriptor('target', ['T'], { pendingCapacityUsed: 10 })],
    'T',
    'Unavailable'
  ],
  [
    'zero weight with capacity',
    [descriptor('target', ['T'], { placementWeight: 0 })],
    'T',
    'Unavailable'
  ],
  [
    'multiple implicit types including full U',
    [descriptor('t', ['T']), descriptor('u', ['U'], { activeCapacityUsed: 10 })],
    undefined,
    'InvalidOperation'
  ]
]) {
  test(`cold type classification: ${name}`, async () => {
    const runtime = fixture(targets);
    await assert.rejects(
      runtime.request(type),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind[kind]
    );
    assert.equal(runtime.submissions(), 0);
  });
}

for (const type of ['T', undefined]) {
  test(`cold type classification: remote-only T succeeds (explicit=${type !== undefined})`, async () => {
    const runtime = fixture([descriptor('target', ['T'])]);
    await runtime.send(type);
    assert.equal(runtime.submissions(), 1);
    assert.equal(runtime.selectedType(), 'T');
  });
}
