const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ServiceTopologyRegistry
} = require('../../packages/framework/dist/runtime/foundation/service-topology-registry');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const constants = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  ReadyDomain
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  ZLinkMeshCompletionTable
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');

function descriptor(rid, overrides = {}) {
  return {
    meshName: 'mesh',
    nodeRoutingId: rid,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `tcp://${rid}`,
    channels: [],
    state: 'serving',
    securityIdentity: 'test',
    applicationVersion: 0n,
    protocolCapabilities: [constants.SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 1,
    activeCapacityLimit: 10,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0,
    ...overrides
  };
}

test('node-direct role and readiness read topology ownership without snapshots', () => {
  const topology = new ServiceTopologyRegistry(descriptor('source'));
  assert.equal(topology.admit(descriptor('target'), 'route'), 'admitted');
  assert.equal(
    topology.admit(descriptor('client', { objectRole: 'client' }), 'client-route'),
    'admitted'
  );
  const runtime = Object.create(RawServiceMeshRuntime.prototype);
  runtime.topology = topology;
  runtime.descriptor = descriptor('source');
  topology.knownDescriptor = () => {
    throw new Error('Role predicate must not clone a descriptor.');
  };
  topology.localDescriptor = () => {
    throw new Error('Role predicate must not clone the local descriptor.');
  };
  topology.peer = () => {
    throw new Error('Readiness predicate must not clone a peer.');
  };
  assert.equal(runtime.isObjectClientNodeDirectTarget('target'), false);
  assert.equal(runtime.isObjectClientNodeDirectTarget('client'), true);
  assert.equal(runtime.isObjectClientNodeDirectTarget('source'), false);
  assert.equal(runtime.isObjectClientNodeDirectTarget('missing'), false);
  assert.equal(runtime.isPeerRouteReady('target', 1n), true);
  assert.equal(runtime.isPeerRouteReady('target', 2n), false);
  const reply = Promise.resolve([]);
  runtime.router = { request: () => reply };
  assert.equal(runtime.requestService('target', [], 10), reply);
  assert.equal(topology.disconnect('target', 'route'), true);
  assert.equal(runtime.isPeerRouteReady('target'), false);
});

test('multipart ingress materializes one final part array and retains borrowed payload views', () => {
  const frame = wire.encodeMultipartApplicationPayload(
    [Buffer.from('header'), Buffer.from('payload')],
    constants.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
    constants.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
  );
  const record = {
    owner: 'target',
    domain: 'application',
    sourceRoutingId: 'source',
    parts: [wire.encodeNodeSendHeader(), frame]
  };
  let claimed = false;
  const backend = new ZLinkNodeRawMeshBackend('mesh', 'target', {});
  backend.runtime = {
    mailbox: {
      tryClaim() {
        if (claimed) return undefined;
        claimed = true;
        return { owner: 'target', records: [record] };
      },
      pendingMessages: () => 0,
      release() {},
      releaseClaimedPayload() {}
    }
  };
  const ready = backend.createReadyBatch(1);
  backend.drainReady(ReadyDomain.Application, ready);
  const claim = ready.takeClaim(0);
  const receive = backend.createReceiveBatch(1, 2);
  const originalArray = global.Array;
  let arrays = 0;
  global.Array = new Proxy(originalArray, {
    construct(target, args, newTarget) {
      if (new Error().stack.includes('decodeMultipart')) arrays++;
      return Reflect.construct(target, args, newTarget);
    }
  });
  let result;
  try {
    result = claim.recvBatch(receive);
  } finally {
    global.Array = originalArray;
  }
  assert.equal(arrays, 1, 'Decode directly into the final Message array.');
  assert.equal(result.records.length, 1);
  assert.deepEqual(
    result.records[0].parts.map((part) => part.getString()),
    ['header', 'payload']
  );
  assert.equal(result.records[0].parts[1].data().buffer, frame.buffer);
  claim.release();
});

test('completion transfers an already managed part array without rebuilding it', async () => {
  const table = new ZLinkMeshCompletionTable();
  const id = { high: 1n, low: 7n };
  const parts = [ZLinkBufferMessage.fromOwned(Buffer.from('reply'))];
  const pending = table.submit(() => id).pending;
  table.complete({
    operationId: id,
    terminalResult: 0,
    failureErrno: 0,
    operationKind: 0,
    kindData: null,
    parts
  });
  const completion = await pending;
  assert.equal(completion.parts, parts);
  assert.equal(completion.parts[0].getString(), 'reply');
});
