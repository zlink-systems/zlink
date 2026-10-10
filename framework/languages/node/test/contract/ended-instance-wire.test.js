const assert = require('node:assert/strict');
const test = require('node:test');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const generated = require('../../packages/framework/dist/runtime/protocol/service_wire_codec.generated');
const {
  ServiceWireProtocolError
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');

const target = {
  targetNodeRid: 'target',
  targetNodeGeneration: 7n,
  targetSpotId: 'room',
  targetMeshName: 'game',
  stableType: 'Room',
  descriptorVersion: 'v1'
};

test('kind 2 uses the canonical route bytes including Mesh and route deadline', () => {
  const header = wire.encodeInstanceSpotActivationHeader(
    target,
    3n,
    'source',
    undefined,
    'request',
    { high: 3n, low: 9n },
    123456n,
    9n
  );
  const route = generated.encodeInstanceRouteV1(
    {
      routeKind: 'coldActivation',
      targetNodeRid: Buffer.from('target'),
      targetNodeGeneration: 7n,
      targetSpotId: 'room',
      targetMeshName: 'game',
      stableType: 'Room',
      targetDescriptorVersion: 'v1',
      deadlineUnixMs: 123456n
    },
    {}
  );
  assert.deepEqual(header.subarray(5, 5 + route.length), Buffer.from(route));
  // Wire schema order, shared by C++, .NET and JVM; independent of both encoders.
  assert.equal(
    header.toString('hex'),
    '5a4d01270002002906746172676574000000000000000704726f6f6d0467616d65' +
      '04526f6f6d027631000000000001e240000000000000000306736f757263650002' +
      '000000000000000300000000000000090000000000000009'
  );
  const decoded = wire.decodeStatefulHeader(header);
  assert.equal(decoded.activation, 'missing');
  assert.deepEqual(decoded.target, target);
  assert.equal(decoded.deadlineUnixMs, 123456n);
  // Canonical command 39 has no second deadline after the operation identity.
  assert.equal(header.length, 5 + route.length + 8 + 1 + 6 + 1 + 1 + 16 + 8);
});

test('kind 2 malformed canonical route is rejected as protocolError before authority access', async () => {
  const header = wire.encodeInstanceSpotActivationHeader(
    target,
    3n,
    'source',
    undefined,
    'send',
    { high: 3n, low: 9n },
    123456n
  );
  // An empty Mesh violates the canonical text bound inside the route.
  const malformed = Buffer.from(header);
  const meshOffset = 5 + 3 + 1 + 6 + 8 + 1 + 4;
  malformed[meshOffset] = 0;
  assert.throws(() => wire.decodeStatefulHeader(malformed), ServiceWireProtocolError);
  let ingress;
  const runtime = new ServiceStatefulRuntime(
    {
      setServiceIngress(handler) {
        ingress = handler;
      }
    },
    'target',
    7n
  );
  let reads = 0;
  runtime.registerAsyncInstanceActivationAuthority({
    async read() {
      ++reads;
      throw new Error('unexpected authority read');
    }
  });
  try {
    assert.equal(
      await ingress({
        command: wire.M6bServiceWireCommand.instanceSpot,
        flags: 0,
        sourceRoutingId: 'source',
        parts: [malformed, Buffer.alloc(0)]
      }),
      'protocolError'
    );
    assert.equal(reads, 0);
  } finally {
    runtime.close();
  }
});
