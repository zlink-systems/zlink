'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ZLinkNodeRawMeshBackend,
  encodeMultipartApplicationFrameNative
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { OperationRegistry } = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY,
  ServiceWireCommand
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

const packet = 'ZLinkFrameworkMultipart';
const contentType = 'application/x-zlink-multipart';

function serviceDescriptor(nodeRoutingId) {
  return {
    meshName: 'native-frame',
    nodeRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://${nodeRoutingId}`,
    channels: [{ name: 'test', weight: 100 }],
    state: 'serving',
    securityIdentity: 'test',
    applicationVersion: 1n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

function createBackend(submitSend, request) {
  const router = {
    setRoutingId() {}, setReceiveFlowState() {}, bind() {}, unbind() {}, connect() {},
    disconnect() {}, connectToRoutingId() {}, disconnectRid() {}, close() {},
    localEndpoint: () => 'inproc://source',
    monitor: () => ({ statusReady: () => true, drain: () => 0, close() {} }),
    receive: () => undefined,
    send: async () => {},
    submitSend,
    request
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: serviceDescriptor('source'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  raw.start();
  const backend = new ZLinkNodeRawMeshBackend(
    'native-frame', 'source', {}, undefined, undefined, undefined, undefined, { raw }
  );
  function admitPeer() {
    raw.topology.admit(
      serviceDescriptor('peer'), 'connection', undefined, undefined, ServiceWireCommand.admit
    );
    raw.liveness.admit('peer', 'connection', 0);
    const probe = raw.liveness.tick(0).probes[0];
    raw.liveness.acknowledge('peer', 'connection', probe.probeId, 0);
  }
  return { backend, raw, admitPeer };
}

function observeNativeCloses(action) {
  const allocate = zlink.Message.allocate;
  let closeCalls = 0;
  zlink.Message.allocate = size => {
    const message = allocate.call(zlink.Message, size);
    const close = message.close.bind(message);
    message.close = () => { closeCalls++; close(); };
    return message;
  };
  try {
    return action(() => closeCalls);
  } finally {
    zlink.Message.allocate = allocate;
  }
}

for (const parts of [
  [Buffer.alloc(0)],
  [Buffer.from('one')],
  [Buffer.alloc(0), Buffer.from('two')],
  [Buffer.from('one'), Buffer.alloc(0), '세 번째']
]) {
  test(`native service frame is byte-identical for ${parts.length} multipart parts`, () => {
    const expected = wire.encodeMultipartApplicationPayload(
      parts.map(part => typeof part === 'string' ? Buffer.from(part) : part), packet, contentType
    );
    const native = encodeMultipartApplicationFrameNative(parts);
    try {
      assert.ok(native instanceof zlink.Message);
      assert.deepEqual(native.data(), expected);
    } finally {
      native.close();
    }
  });
}

test('native service frame closes its allocation once when multipart writing throws', () => {
  const allocate = zlink.Message.allocate;
  let closeCalls = 0;
  zlink.Message.allocate = size => {
    const message = allocate.call(zlink.Message, size);
    const close = message.close.bind(message);
    message.close = () => { closeCalls++; close(); };
    return message;
  };
  try {
    let reads = 0;
    const throwingPart = new Proxy(new Uint8Array([1]), {
      get(target, property) {
        if (property === 'byteLength' && ++reads > 1) throw new Error('mid-write');
        return Reflect.get(target, property, target);
      }
    });
    assert.throws(() => encodeMultipartApplicationFrameNative([throwingPart]), /mid-write/);
    assert.equal(closeCalls, 1);
  } finally {
    zlink.Message.allocate = allocate;
  }
});

test('error reply keeps its header-only M6A bytes', () => {
  assert.deepEqual(
    wire.encodeReplyHeader(1n, 107, 33),
    Buffer.from([90, 77, 1, 20, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 107, 0, 0, 0, 33])
  );
});

test('native service frame closes once when binding submission throws', () => {
  const fixture = createBackend(() => { throw new Error('binding rejected'); });
  fixture.admitPeer();
  try {
    observeNativeCloses(closeCalls => {
      assert.throws(() => fixture.backend.sendToNode('peer', Buffer.from('payload')), /binding rejected/);
      assert.equal(closeCalls(), 1);
    });
  } finally {
    fixture.raw.close();
  }
});

test('unavailable route keeps the Buffer frame allocation strategy', () => {
  const fixture = createBackend(() => { throw new Error('no route must not submit'); });
  try {
    observeNativeCloses(closeCalls => {
      assert.notEqual(fixture.backend.sendToNode('peer', Buffer.from('payload')), SubmitResult.Ok);
      assert.equal(closeCalls(), 0);
    });
  } finally {
    fixture.raw.close();
  }
});

test('cancelled native request closes once before binding submission', () => {
  let requestCalls = 0;
  const fixture = createBackend(undefined, () => { requestCalls++; throw new Error('cancelled request must not be submitted'); });
  fixture.admitPeer();
  const registry = new OperationRegistry();
  const pending = registry.register(10000);
  pending.promise.catch(() => {});
  registry.cancel(pending.id, 'cancel before request submit');
  try {
    observeNativeCloses(closeCalls => {
      fixture.backend.requestToNode('peer', Buffer.from('payload'), {
        timeoutMs: 1000,
        requestOperation: pending
      });
      assert.equal(requestCalls, 0);
      assert.equal(closeCalls(), 1);
    });
  } finally {
    fixture.raw.close();
  }
});
