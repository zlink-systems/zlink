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

test('completion retention keeps owned framework buffers and copies only native message storage', async () => {
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
  const nativeCompletion = table.submit(() => ({ high: 1n, low: 2n }));
  try {
    table.complete(completionRecord(2n, [native]));
  } finally {
    native.close();
  }
  const retainedNative = await nativeCompletion;
  try {
    assert.equal(retainedNative.parts[0].data().toString(), 'native completion');
  } finally {
    closeMeshCompletion(retainedNative);
    table.dispose();
  }
});

test('generic channel send writes one valid multipart application frame before source close', async () => {
  let replyFrame;
  const router = {
    setRoutingId() {}, setReceiveFlowState() {}, bind() {}, unbind() {}, connect() {}, disconnect() {},
    connectToRoutingId() {}, disconnectRid() {}, close() {}, localEndpoint: () => 'inproc://source-node',
    monitor: () => ({ statusReady: () => true, drain: () => 0, close() {} }),
    receive: () => undefined, send: async () => {},
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
  const target = descriptor('target-node');
  raw.topology.admit(target, 'connection');
  raw.liveness.admit(target.nodeRoutingId, 'connection', 0);
  raw.liveness.requestProbe(target.nodeRoutingId, 'connection', 0);
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
  const claim = raw.mailbox.tryClaim('application', 1, 1024);
  try {
    assert.ok(claim);
    const frame = claim.records[0].parts[1];
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
    if (claim !== undefined) {
      for (const record of claim.records) {
        raw.mailbox.releaseClaimedPayload(record);
        record.applicationJob?.close();
      }
      raw.mailbox.release(claim);
    }
    raw.close();
  }
});
