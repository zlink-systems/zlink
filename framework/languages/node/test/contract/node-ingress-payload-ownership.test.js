const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const { allocatePayloadBytes } = require('../../packages/framework/dist/contracts/Common/payload-slab');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ZLinkMeshCompletionTable,
  closeMeshCompletion
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  decodeFrameworkTypedPayloadMessage
} = require('../../packages/framework/dist/runtime/messaging/payload-codec');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  decodeApplicationPayloadView,
  decodeNodeRequestHeader,
  encodeApplicationPayload,
  encodeMultipartApplicationPayload,
  encodeReplyHeader
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const creationPayloadCodec =
  require('../../packages/framework/dist/runtime/messaging/creation-payload-codec');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY,
  ServiceWireCommand
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

function descriptor(nodeRoutingId, channels = []) {
  return {
    meshName: 'payload-ownership',
    nodeRoutingId,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://${nodeRoutingId}`,
    channels,
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

function completionRecord(low, parts) {
  return {
    operationId: { high: 1n, low },
    terminalResult: 0,
    failureErrno: 0,
    operationKind: 1,
    kindData: null,
    parts
  };
}

function multipartParts(payload) {
  let offset = 0;
  const count = payload.readUInt32BE(offset);
  offset += 4;
  const parts = [];
  for (let index = 0; index < count; index++) {
    const length = payload.readUInt32BE(offset);
    offset += 4;
    parts.push(payload.subarray(offset, offset + length));
    offset += length;
  }
  assert.equal(offset, payload.length);
  return parts;
}

test('completion retention keeps managed and binding payload views after source close', async () => {
  const table = new ZLinkMeshCompletionTable();
  const ownedBytes = Buffer.from('managed completion');
  const owned = ZLinkBufferMessage.fromOwned(ownedBytes);
  const ownedCompletion = table.submit(() => ({ high: 1n, low: 1n }));
  table.complete(completionRecord(1n, [owned]));
  owned.close();
  const retainedOwned = await ownedCompletion;
  try {
    assert.strictEqual(retainedOwned.parts[0].data(), ownedBytes);
    assert.equal(retainedOwned.parts[0].data().toString(), 'managed completion');
  } finally {
    closeMeshCompletion(retainedOwned);
  }

  const native = zlink.Message.from(Buffer.from('native completion'));
  const nativeView = native.data();
  const nativeCompletion = table.submit(() => ({ high: 1n, low: 2n }));
  try {
    table.complete(completionRecord(2n, [native]));
  } finally {
    native.close();
  }
  const retainedNative = await nativeCompletion;
  try {
    nativeView[0] = 'N'.charCodeAt(0);
    assert.equal(retainedNative.parts[0].data()[0], nativeView[0]);
    assert.equal(retainedNative.parts[0].data().toString(), 'Native completion');
  } finally {
    closeMeshCompletion(retainedNative);
    table.dispose();
  }
});

test('native received binary payload has one retained view across completion and reply decoding', async () => {
  const {
    encodeChannelReplyParts,
    decodeChannelReply
  } = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const received = new zlink.Received();
  const table = new ZLinkMeshCompletionTable();
  let completion;
  try {
    const endpoint = 'inproc://native-payload-retention';
    router.bind(endpoint);
    dealer.connect(endpoint);
    const parts = encodeChannelReplyParts(
      { channelName: 'mesh', messageName: 'Reply', correlationId: 'a'.repeat(32) },
      Buffer.alloc(1024, 7)
    );
    dealer.send().message(parts[0]).message(parts[1]).submit_sync();
    assert.equal(router.recv(received), true);
    const nativeView = received.parts[1].data();
    const pending = table.submit(() => ({ high: 1n, low: 3n }));
    table.complete(completionRecord(3n, received.parts));
    const directReply = decodeChannelReply(received.parts);
    received.close();
    completion = await pending;
    const completionReply = decodeChannelReply(completion.parts);
    assert.equal(directReply[0], 7);
    assert.equal(completionReply[0], 7);
    nativeView[0] = 9;
    assert.equal(directReply[0], 9);
    assert.equal(completionReply[0], 9);
    assert.strictEqual(completionReply, completion.parts[1].data());
  } finally {
    if (completion !== undefined) closeMeshCompletion(completion);
    table.dispose();
    received.close();
    dealer.close();
    router.close();
    context.close();
  }
});

test('generic channel send writes one valid multipart application frame before source close', async () => {
  let replyFrame;
  let sentParts;
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    bind() {},
    unbind() {},
    connect() {},
    disconnect() {},
    connectToRoutingId() {},
    disconnectRid() {},
    close() {},
    localEndpoint: () => 'inproc://source-node',
    monitor: () => ({ statusReady: () => true, drain: () => 0, close() {} }),
    receive: () => undefined,
    send: async (_target, parts) => {
      sentParts = parts;
    },
    submitSend: (_target, parts) => {
      sentParts = parts;
      return { result: SubmitResult.Ok, admitted: Promise.resolve() };
    },
    request: (_target, parts, _timeout, onReply, onFailed) =>
      Promise.resolve([encodeReplyHeader(decodeNodeRequestHeader(parts[0])), replyFrame]).then(
        onReply,
        onFailed
      )
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source-node', [{ name: 'orders', weight: 100 }]),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  raw.start();
  // Channel messaging §2 requires an admitted remote channel target.
  const target = descriptor('target-node', [{ name: 'orders', weight: 100 }]);
  raw.topology.admit(target, 'connection', undefined, undefined, ServiceWireCommand.admit);
  raw.liveness.admit(target.nodeRoutingId, 'connection', 0);
  const probe = raw.liveness.tick(0).probes[0];
  raw.liveness.acknowledge(target.nodeRoutingId, 'connection', probe.probeId, 0);
  const backend = new ZLinkNodeRawMeshBackend(
    'payload-ownership',
    'source-node',
    {},
    undefined,
    undefined,
    undefined,
    undefined,
    { raw }
  );
  const source = zlink.Message.from(Buffer.from('first'));
  try {
    assert.equal(
      await backend.sendToChannel('orders', [source, Buffer.from('second')]),
      SubmitResult.Ok
    );
  } finally {
    source.close();
  }
  try {
    assert.ok(sentParts);
    const frame = sentParts[1];
    const application = decodeApplicationPayloadView(frame);
    assert.equal(application.packetName, 'ZLinkFrameworkMultipart');
    assert.equal(application.contentType, 'application/x-zlink-multipart');
    assert.deepEqual(
      multipartParts(application.payload).map((part) => part.toString()),
      ['first', 'second']
    );

    replyFrame = encodeApplicationPayload({
      packetName: 'reply',
      contentType: 'application/octet-stream',
      payload: Buffer.from('view')
    });
    const request = raw.requestToNode(
      target.nodeRoutingId,
      encodeMultipartApplicationPayload(
        [Buffer.from('request')],
        'ZLinkFrameworkMultipart',
        'application/x-zlink-multipart'
      ),
      100
    );
    const reply = await request.promise;
    assert.strictEqual(reply.payload.payload.buffer, replyFrame.buffer);
    assert.equal(reply.payload.payload.toString(), 'view');
  } finally {
    raw.close();
  }
});

test('raw router receive retains binding payload storage and replies on the delivered route after close', async () => {
  const {
    ZLinkNodeRawBindingPort
  } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
  const context = zlink.createContext();
  const host = new ZLinkNodeRawBindingPort(context).createHost();
  const router = host.createRouter();
  const dealer = host.createDealer();
  const payload = Buffer.alloc(1024, 5);
  const originalFrom = Buffer.from;
  let payloadCopies = 0;
  try {
    const endpoint = 'inproc://raw-receive-retention';
    router.bind(endpoint);
    dealer.connect(endpoint);
    const reply = dealer.request([Buffer.from('header'), payload], 5000);
    let record;
    for (let attempt = 0; attempt < 200 && record === undefined; attempt++) {
      Buffer.from = function (...args) {
        const result = originalFrom.apply(this, args);
        if (result.length === payload.length) payloadCopies++;
        return result;
      };
      try {
        record = router.receive(true);
      } finally {
        Buffer.from = originalFrom;
      }
      if (record === undefined) await new Promise((resolve) => setImmediate(resolve));
    }
    assert.ok(record !== undefined, 'router received the request');
    // The record keeps the binding's own data() storage: no Framework copy.
    assert.equal(payloadCopies, 0);
    assert.equal(record.parts[1].length, payload.length);
    record.close();
    assert.equal(record.parts[1][0], 5);
    assert.equal(record.parts[1][1023], 5);
    // The reply addresses the RoutingId the record was delivered with; it
    // does not rebuild a binding RoutingId per reply.
    const originalRoutingIdFrom = zlink.RoutingId.from;
    let routingIds = 0;
    zlink.RoutingId.from = function (...args) {
      routingIds++;
      return originalRoutingIdFrom.apply(this, args);
    };
    try {
      record.reply([Buffer.from('accepted')]);
    } finally {
      zlink.RoutingId.from = originalRoutingIdFrom;
    }
    assert.equal(routingIds, 0);
    void reply.catch(() => undefined);
  } finally {
    Buffer.from = originalFrom;
    host.close();
    context.close();
  }
});

test('local Spot and Node messages never expose payload-slab backing', async (t) => {
  const {
    ZLinkNodeBackendAdapterFactory,
    ZLinkMeshDispatchPump
  } = require('../../packages/framework/dist/runtime/backend');
  const {
    ApplicationJobQueue,
    resolveApplicationJobQueueConfiguration
  } = require('../../packages/framework/dist/runtime/host/application-job-queue');
  const {
    ReceiveKind
  } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
  const suffix = `${process.pid}-${Date.now()}`;
  const factory = new ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}));
  const channelName = 'local-payload-events';
  const nodeId = `local-payload-ownership-${suffix}`;
  const node = factory.createMeshAdapter().createMeshNode(context, {
    meshName: nodeId,
    routingId: nodeId,
    applicationJobQueue: queue
  });
  node.addChannelName(channelName);
  const table = new ZLinkMeshCompletionTable();
  const sendReceived = deferred();
  const requestReceived = deferred();
  const multicastReceived = deferred();
  const nodeRequestReceived = deferred();
  const slabBuffers = new Set();
  const allocateUnsafe = Buffer.allocUnsafe;
  t.mock.method(Buffer, 'allocUnsafe', function (size, ...args) {
    const bytes = allocateUnsafe.call(Buffer, size, ...args);
    if (size === 64 * 1024) slabBuffers.add(bytes.buffer);
    return bytes;
  });
  const messageFrameSlabs = new Map();
  const replyFrameSlabs = new Map();
  const captureFrameSlab = (target, key) => {
    const slab = allocatePayloadBytes(1).buffer;
    slabBuffers.add(slab);
    target.set(key, slab);
    return slab;
  };
  let pump;
  node.setBind(`inproc://local-payload-ownership-${suffix}`);
  node.start();
  const spot = node.entrySpot();
  const localSpot = node.createSpot();
  localSpot.setSubscription(channelName, '#');
  const status = node.status();
  node.setCompletionHandler((terminal, materialize) => table.complete(terminal, materialize));
  pump = new ZLinkMeshDispatchPump(node, {
    applicationJobQueue: queue,
    dispatch(_owner, record) {
      try {
        const data = record.parts[0].data();
        const view = framework.ZLinkEncodedPayload.fromOwned(data).view();
        const serializerView = decodeFrameworkTypedPayloadMessage(
          record.parts[0],
          new Map([
            [
              'application/x-local-payload-ownership',
              {
                deserialize(payload) {
                  return payload.view();
                }
              }
            ]
          ]),
          Object,
          'application/x-local-payload-ownership'
        );
        const expectedByKind = new Map([
          [ReceiveKind.SpotSend, 'local send'],
          [ReceiveKind.SpotRequest, 'local request'],
          [ReceiveKind.SpotMulticast, 'local multicast'],
          [ReceiveKind.NodeRequest, 'local node request']
        ]);
        const expected = expectedByKind.get(record.kind);
        assert.ok(expected, `unexpected local receive kind ${record.kind}`);
        const frameTimeSlab = messageFrameSlabs.get(record.kind);
        assert.ok(frameTimeSlab, `frame-time slab captured for receive kind ${record.kind}`);
        assertNotPayloadSlabBacked(data, 'Message.data()', expected, frameTimeSlab, slabBuffers);
        assertNotPayloadSlabBacked(
          view,
          'ZLinkEncodedPayload.view()',
          expected,
          frameTimeSlab,
          slabBuffers
        );
        assertNotPayloadSlabBacked(
          serializerView,
          'application serializer view()',
          expected,
          frameTimeSlab,
          slabBuffers
        );
        if (record.kind === ReceiveKind.SpotSend) {
          assert.equal(data.toString(), expected);
          sendReceived.resolve();
          return;
        }
        if (record.kind === ReceiveKind.SpotRequest) {
          assert.equal(data.toString(), expected);
          requestReceived.resolve();
          captureFrameSlab(replyFrameSlabs, 'spot');
          assert.equal(record.reply([Buffer.from('local reply')]), SubmitResult.Ok);
          return;
        }
        if (record.kind === ReceiveKind.SpotMulticast) {
          assert.equal(data.toString(), expected);
          multicastReceived.resolve();
          return;
        }
        if (record.kind === ReceiveKind.NodeRequest) {
          assert.equal(data.toString(), expected);
          nodeRequestReceived.resolve();
          captureFrameSlab(replyFrameSlabs, 'node');
          assert.equal(record.reply([Buffer.from('local node reply')]), SubmitResult.Ok);
          return;
        }
        assert.fail(`unexpected local receive kind ${record.kind}`);
      } catch (error) {
        for (const observed of [
          sendReceived,
          requestReceived,
          multicastReceived,
          nodeRequestReceived
        ]) {
          observed.reject(error);
        }
      }
    },
    reportError(error) {
      for (const observed of [
        sendReceived,
        requestReceived,
        multicastReceived,
        nodeRequestReceived
      ]) {
        observed.reject(error);
      }
    }
  });
  pump.start();
  try {
    captureFrameSlab(messageFrameSlabs, ReceiveKind.SpotSend);
    await spot.sendToSpot(
      status.routingId,
      status.routingId,
      status.lifecycleGeneration,
      [Buffer.from('local send')],
      { entrySpot: true }
    );
    await sendReceived.promise;
    captureFrameSlab(messageFrameSlabs, ReceiveKind.SpotRequest);
    const request = table.submit((completion) =>
      spot.requestToSpot(
        status.routingId,
        status.routingId,
        status.lifecycleGeneration,
        [Buffer.from('local request')],
        { entrySpot: true },
        completion
      )
    );
    await requestReceived.promise;
    const completion = await request;
    try {
      assert.equal(completion.parts[0].data().toString(), 'local reply');
      const bytes = completion.parts[0].data();
      assertNotPayloadSlabBacked(
        bytes,
        'request reply Message.data()',
        'local reply',
        replyFrameSlabs.get('spot'),
        slabBuffers
      );
      assertNotPayloadSlabBacked(
        framework.ZLinkEncodedPayload.fromOwned(bytes).view(),
        'request reply ZLinkEncodedPayload.view()',
        'local reply',
        replyFrameSlabs.get('spot'),
        slabBuffers
      );
    } finally {
      closeMeshCompletion(completion);
    }
    captureFrameSlab(messageFrameSlabs, ReceiveKind.SpotMulticast);
    await localSpot.publish(channelName, 'updates', [Buffer.from('local multicast')]);
    await multicastReceived.promise;
    captureFrameSlab(messageFrameSlabs, ReceiveKind.NodeRequest);
    const nodeRequest = table.submit((completion) =>
      node.requestToNode(
        status.routingId,
        [Buffer.from('local node request')],
        { timeoutMs: 1000 },
        completion
      )
    );
    await nodeRequestReceived.promise;
    const nodeCompletion = await nodeRequest;
    try {
      assert.equal(nodeCompletion.parts[0].data().toString(), 'local node reply');
      const bytes = nodeCompletion.parts[0].data();
      assertNotPayloadSlabBacked(
        bytes,
        'local node reply Message.data()',
        'local node reply',
        replyFrameSlabs.get('node'),
        slabBuffers
      );
      assertNotPayloadSlabBacked(
        framework.ZLinkEncodedPayload.fromOwned(bytes).view(),
        'local node reply ZLinkEncodedPayload.view()',
        'local node reply',
        replyFrameSlabs.get('node'),
        slabBuffers
      );
    } finally {
      closeMeshCompletion(nodeCompletion);
    }
  } finally {
    await pump?.dispose();
    localSpot.close();
    table.dispose();
    await node.close();
    await context.dispose();
  }
});

test('retained creation payloads keep serializer views off the payload slab', () => {
  const frameTimeSlab = allocatePayloadBytes(1).buffer;
  const serializer = {
    canSerialize(value) {
      return value?.kind === 'application';
    },
    serialize(value) {
      return framework.ZLinkEncodedPayload.from(Buffer.from(`wire:${value.kind}`));
    },
    deserialize(payload) {
      const view = payload.view();
      assert.equal(view.byteLength, Buffer.byteLength('wire:application'));
      assert.notStrictEqual(view.buffer, frameTimeSlab);
      assert.notStrictEqual(view.buffer, allocatePayloadBytes(1).buffer);
      return { kind: 'application', decoded: true };
    }
  };
  const registry = new Map([['application/x-local-payload-ownership', serializer]]);
  const frame = creationPayloadCodec.encodeFrameworkCreationPayload(
    { kind: 'application' },
    registry
  );

  assert.notStrictEqual(frame.buffer, frameTimeSlab);
  assert.deepEqual(
    creationPayloadCodec.decodeFrameworkCreationPayload(frame, registry),
    { kind: 'application', decoded: true }
  );
});

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  void promise.catch(() => undefined);
  return { promise, resolve, reject };
}

function assertNotPayloadSlabBacked(bytes, label, expected, frameTimeSlab, slabBuffers) {
  const currentSlab = allocatePayloadBytes(1).buffer;
  slabBuffers.add(currentSlab);
  assert.equal(bytes.byteLength, Buffer.byteLength(expected), `${label} has the expected byteLength`);
  assert.notStrictEqual(bytes.buffer, frameTimeSlab, `${label} avoids its frame-time slab`);
  assert.notStrictEqual(bytes.buffer, currentSlab, `${label} avoids the current payload slab`);
  for (const slab of slabBuffers) {
    assert.notStrictEqual(bytes.buffer, slab, `${label} is not backed by our 64 KiB slab`);
  }
}
