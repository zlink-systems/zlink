const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { ZLinkMeshCompletionTable, closeMeshCompletion } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  decodeApplicationPayloadView,
  decodeNodeRequestHeader,
  encodeApplicationPayload,
  encodeMultipartApplicationPayload,
  encodeReplyHeader
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { SERVICE_WIRE_REQUIRED_CAPABILITY } = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

function descriptor(nodeRoutingId, channels = []) {
  return {
    meshName: 'payload-ownership', nodeRoutingId, lifecycleGeneration: 1n,
    descriptorRevision: 1n, advertisedEndpoint: `inproc://${nodeRoutingId}`,
    channels, state: 'serving', securityIdentity: 'test', applicationVersion: 1n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
    placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 10,
    activeCapacityUsed: 0, pendingCapacityUsed: 0
  };
}

function completionRecord(low, parts) {
  return {
    operationId: { high: 1n, low }, terminalResult: 0, failureErrno: 0,
    operationKind: 1, kindData: null, parts
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
  const { encodeChannelReplyParts, decodeChannelReply } = require('../../packages/framework/dist/runtime/channels/channel-envelope');
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
    setRoutingId() {}, setReceiveFlowState() {}, bind() {}, unbind() {}, connect() {}, disconnect() {},
    connectToRoutingId() {}, disconnectRid() {}, close() {}, localEndpoint: () => 'inproc://source-node',
    monitor: () => ({ statusReady: () => true, drain: () => 0, close() {} }),
    receive: () => undefined, send: async (_target, parts) => { sentParts = parts; },
    submitSend: (_target, parts) => {
      sentParts = parts;
      return { result: SubmitResult.Ok, admitted: Promise.resolve() };
    },
    request: async (_target, parts) => [
      encodeReplyHeader(decodeNodeRequestHeader(parts[0])), replyFrame
    ]
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source-node', [{ name: 'orders', weight: 100 }]),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  raw.start();
  // Channel messaging §2 requires an admitted remote channel target.
  const target = descriptor('target-node', [{ name: 'orders', weight: 100 }]);
  raw.topology.admit(target, 'connection');
  raw.liveness.admit(target.nodeRoutingId, 'connection', 0);
  const probe = raw.liveness.tick(0).probes[0];
  raw.liveness.acknowledge(target.nodeRoutingId, 'connection', probe.probeId, 0);
  const backend = new ZLinkNodeRawMeshBackend('payload-ownership', 'source-node', {});
  backend.runtime = raw;
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
    assert.deepEqual(multipartParts(application.payload).map(part => part.toString()), ['first', 'second']);

    replyFrame = encodeApplicationPayload({
      packetName: 'reply', contentType: 'application/octet-stream', payload: Buffer.from('view')
    });
    const request = raw.requestToNode(
      target.nodeRoutingId,
      encodeMultipartApplicationPayload([Buffer.from('request')], 'ZLinkFrameworkMultipart', 'application/x-zlink-multipart'),
      100
    );
    const reply = await request.promise;
    assert.strictEqual(reply.payload.payload.buffer, replyFrame.buffer);
    assert.equal(reply.payload.payload.toString(), 'view');
  } finally {
    raw.close();
  }
});
