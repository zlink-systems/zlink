const assert = require('node:assert/strict');
const test = require('node:test');

test('ingress terminal releases a permit held at handler entry by its queue owner', () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n),
    () => 0,
    () => true
  );
  const permit = queue.acquireOrResume(assert.fail, assert.fail, undefined, 'remote');
  const owner = ApplicationIngressRecordOwner.create(queue, permit, { close() {} });
  const job = owner.takeInitial('application');
  job.releaseBeforeHandler();
  assert.equal(queue.snapshot().permitsInUse, 1n);
  owner.close();
  job.close();
  assert.equal(queue.snapshot().permitsInUse, 0n);
  job.releaseBeforeHandler();
  job.releaseAfterInternalProcessing();
  assert.equal(queue.snapshot().permitsInUse, 0n);
});
const {
  ZLinkMeshCompletionTable
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { createHook } = require('node:async_hooks');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  ApplicationIngressRecordOwner
} = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
const {
  ZLinkChannelDispatchServices
} = require('../../packages/framework/dist/runtime/channels/channel-dispatch-services');
const {
  createFrameworkRegistration
} = require('../../packages/framework/dist/runtime/configuration');
const {
  encodeChannelEnvelopeParts,
  encodeChannelReplyParts,
  decodeChannelReply,
  decodeChannelHeader
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  runWithApplicationJobPermit
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');

test('managed and binding binary replies retain payload views after source close', () => {
  const header = { channelName: 'mesh', messageName: 'Reply', correlationId: 'a'.repeat(32) };
  const payload = Buffer.alloc(1024, 7);
  const parts = encodeChannelReplyParts(header, payload);
  const managed = parts.map((bytes) => ZLinkBufferMessage.fromOwned(bytes));
  const reply = decodeChannelReply(managed);
  assert.equal(reply, payload);
  for (const part of managed) part.close();
  assert.equal(reply[0], 7);
  const { Message } = require('@zlink-systems/zlink');
  const generic = [Message.from(parts[0]), Message.from(Buffer.alloc(1024, 8))];
  const nativeBytes = generic[1].data();
  const retained = decodeChannelReply(generic);
  for (const part of generic) part.close();
  assert.equal(retained[0], 8);
  nativeBytes[0] = 9;
  assert.equal(retained[0], 9, 'Retain binding storage without a second payload allocation.');
});

test('rotation accounting preserves exact INT32_MAX capacity and bigint snapshots', () => {
  const {
    MAX_QUEUED_APPLICATION_JOBS
  } = require('../../packages/framework/dist/contracts/Configuration/Registration');
  const count = Number(MAX_QUEUED_APPLICATION_JOBS);
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: BigInt(count) }, () => 1n)
  );
  queue.reserveReceiveRotation(count);
  assert.equal(queue.snapshot().reservedSupplyPermits, BigInt(count));
  assert.equal(queue.snapshot().pressureState, 'paused');
  assert.equal(queue.receiveRotationCapacity(64), 0);
  queue.markQueued();
  assert.equal(queue.snapshot().queuedApplicationJobs, 1n);
  queue.release('queued');
  queue.release('reserved');
  assert.equal(queue.snapshot().permitsInUse, BigInt(count) - 2n);
  assert.equal(queue.snapshot().peakPermitsInUse, BigInt(count));
  queue.resetMetrics();
  assert.equal(queue.snapshot().peakPermitsInUse, BigInt(count) - 2n);
  assert.equal(queue.receiveRotationCapacity(64), 2);
});

test('a synchronous permit turn does not allocate operation Promises', async () => {
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const permit = queue.acquireOrResume(assert.fail, assert.fail, undefined, 'remote');
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') promises++;
    }
  });
  hook.enable();
  let result;
  try {
    result = runWithApplicationJobPermit(permit, () => undefined);
  } finally {
    hook.disable();
  }
  await result;
  assert.equal(queue.snapshot().permitsInUse, 0n);
  assert.equal(promises, 0);
});

test('trace-off command headers are prepared once while request fields remain per operation', () => {
  const codecs = { serializers: new Map() };
  const payload = Buffer.alloc(1024);
  const first = encodeChannelEnvelopeParts(
    3,
    'mesh',
    'Command',
    payload,
    undefined,
    undefined,
    codecs,
    undefined,
    false
  );
  const next = encodeChannelEnvelopeParts(
    3,
    'mesh',
    'Command',
    payload,
    undefined,
    undefined,
    codecs,
    undefined,
    false
  );
  assert.equal(next[0], first[0], 'constant command header must be registration-owned storage');
  const firstId = 'a'.repeat(32);
  const nextId = 'b'.repeat(32);
  const request = encodeChannelEnvelopeParts(
    1,
    'mesh',
    'Request',
    payload,
    3000,
    undefined,
    codecs,
    firstId,
    false
  );
  const other = encodeChannelEnvelopeParts(
    1,
    'mesh',
    'Request',
    payload,
    4000,
    undefined,
    codecs,
    nextId,
    false
  );
  assert.notEqual(request[0], other[0]);
  const requestHeader = decodeChannelHeader([ZLinkBufferMessage.fromOwned(request[0])], false);
  const otherHeader = decodeChannelHeader([ZLinkBufferMessage.fromOwned(other[0])], false);
  assert.equal(requestHeader.correlationId, firstId);
  assert.equal(otherHeader.correlationId, nextId);
  assert.notEqual(requestHeader.deadline, otherHeader.deadline);
});

test('a dependency-free handler has a fresh logical scope without scope Promises', async () => {
  const services = new ZLinkChannelDispatchServices(createFrameworkRegistration());
  const instances = [];
  class Handler {
    handle() {
      instances.push(this);
    }
  }
  const handler = services.routeSendHandler(Handler);
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') promises++;
    }
  });
  hook.enable();
  let first;
  let second;
  try {
    first = handler.handle(null, { metadata: new Map() });
    second = handler.handle(null, { metadata: new Map() });
  } finally {
    hook.disable();
  }
  await first;
  await second;
  assert.equal(instances.length, 2);
  assert.notEqual(instances[0], instances[1]);
  assert.equal(promises, 0);
});

test('receive rotation returns capacity at handler entry and retains storage through ingress cleanup', () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 4n }, () => 1n)
  );
  const rotation = queue.createReceiveRotation(3, 'remote');
  rotation.reserve();
  assert.equal(queue.snapshot().reservedSupplyPermits, 3n);
  const permit = rotation.take();
  let closed = 0;
  const owner = ApplicationIngressRecordOwner.create(queue, permit, {
    close() {
      closed++;
    }
  });
  const job = owner.takeInitial('application');
  job.releaseBeforeHandler();
  assert.equal(queue.snapshot().permitsInUse, 2n);
  rotation.releaseUnused();
  assert.equal(queue.snapshot().permitsInUse, 0n);
  rotation.reserve();
  const next = rotation.take();
  assert.notEqual(next, permit, 'an active ingress must retain its reservation storage');
  next.releaseAfterInternalProcessing();
  next.releaseStorage();
  rotation.releaseUnused();
  owner.close();
  assert.equal(closed, 0);
  job.close();
  assert.equal(closed, 1);
  rotation.reserve();
  const recycled = rotation.take();
  assert.equal(recycled, permit);
  recycled.releaseAfterInternalProcessing();
  recycled.releaseStorage();
  rotation.releaseUnused();
  assert.equal(queue.snapshot().permitsInUse, 0n);
});

test('ordinary ingress transfers its existing permit and payload without operation Promises', async () => {
  const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  const descriptor = {
    meshName: 'mesh',
    nodeRoutingId: 'source',
    advertisedEndpoint: 'tcp://127.0.0.1:1',
    channels: [],
    state: 'serving',
    securityIdentity: 'test',
    applicationVersion: 0n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 1,
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    activeCapacityLimit: 10,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
  const runtime = new RawServiceMeshRuntime({ applicationJobQueue: queue, descriptor });
  const peer = { descriptor: { ...descriptor, nodeRoutingId: 'peer' } };
  runtime.topology.admittedPeer = () => peer;
  const record = {
    sourceRid: 'peer',
    sourceRoute: Buffer.from('peer'),
    routeGeneration: 1n,
    parts: [wire.encodeNodeSendHeader(), Buffer.alloc(1024)],
    close() {}
  };
  const permit = queue.acquireOrResume(
    () => assert.fail('capacity is available'),
    assert.fail,
    undefined,
    'remote'
  );
  const owner = ApplicationIngressRecordOwner.create(queue, permit, record);
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') promises++;
    }
  });
  hook.enable();
  let result;
  try {
    result = runtime.processReceived(record, 0, owner);
  } finally {
    hook.disable();
  }
  assert.equal(await result, 'application');
  assert.equal(promises, 0);
  owner.close();
  runtime.mailbox.close();
  assert.equal(queue.snapshot().permitsInUse, 0n);
});

test('immediately admitted node send submits once without allocating a Promise', async () => {
  const runtime = new RawServiceMeshRuntime({
    applicationJobQueue: {},
    descriptor: {
      meshName: 'mesh',
      nodeRoutingId: 'source',
      advertisedEndpoint: 'tcp://127.0.0.1:1',
      channels: [],
      state: 'serving',
      securityIdentity: 'test',
      applicationVersion: 0n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
      objectRole: 'server',
      placementWeight: 1,
      lifecycleGeneration: 1n,
      descriptorRevision: 1n,
      activeCapacityLimit: 10,
      pendingCapacityLimit: 10,
      activeCapacityUsed: 0,
      pendingCapacityUsed: 0
    }
  });
  let submissions = 0;
  const submission = {
    result: 0,
    get admitted() {
      assert.fail('OK must not observe admission');
    }
  };
  runtime.router = {
    submitSend() {
      submissions++;
      return submission;
    },
    send() {
      submissions++;
      return Promise.resolve();
    }
  };
  const backend = new ZLinkNodeRawMeshBackend('mesh', 'source', {});
  backend.runtime = runtime;
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') promises++;
    }
  });
  const payload = Buffer.alloc(1024);
  hook.enable();
  let result;
  try {
    result = backend.sendToNode('peer', [payload]);
  } finally {
    hook.disable();
  }
  assert.equal(await result, 0);
  assert.equal(submissions, 1);
  assert.equal(promises, 0, 'Framework immediate submit must not create operation Promises');
});

function terminal(operationId) {
  return {
    operationId,
    terminalResult: 0,
    failureErrno: 0,
    operationKind: 0,
    kindData: null,
    parts: []
  };
}

test('completion registration reuses abort delivery storage across sequential operations', async () => {
  const table = new ZLinkMeshCompletionTable();
  let firstListener;
  for (let index = 1; index <= 100; index++) {
    const controller = new AbortController();
    let addedListener;
    const add = controller.signal.addEventListener.bind(controller.signal);
    controller.signal.addEventListener = (type, listener, options) => {
      if (type === 'abort') {
        addedListener = listener;
      }
      return add(type, listener, options);
    };
    const operationId = { high: 1n, low: BigInt(index) };
    const pending = table.submit(() => operationId, controller.signal);
    assert.equal(table.pendingCount, 1);
    table.complete(terminal(operationId));
    await pending;
    firstListener ??= addedListener;
    assert.equal(addedListener, firstListener, 'registration must reuse its slot listener');
    assert.equal(table.pendingCount, 0);
    controller.abort(new Error('old signal must not affect a reused slot'));
  }
  table.dispose();
});

test('an aborted submission cannot bind a slot reused by a reentrant submission', async () => {
  const table = new ZLinkMeshCompletionTable();
  const controller = new AbortController();
  const failure = new Error('cancel outer');
  const innerId = { high: 2n, low: 1n };
  let inner;
  const outer = table.submit(() => {
    controller.abort(failure);
    inner = table.submit(() => innerId);
    return { high: 2n, low: 2n };
  }, controller.signal);
  await assert.rejects(outer, (error) => error === failure);
  assert.equal(table.pendingCount, 1);
  table.complete(terminal(innerId));
  await inner;
  assert.equal(table.pendingCount, 0);
  table.dispose();
});

test('completion storage grows without dropping pending identities or aliasing colliding u64 values', async () => {
  const table = new ZLinkMeshCompletionTable();
  const pending = [];
  const ids = [];
  for (let index = 1; index <= 257; index++) {
    // Both words exceed the precise Number range; bucket collisions cannot define identity.
    const operationId = { high: (1n << 63n) + BigInt(index), low: (1n << 63n) + BigInt(index * 2) };
    ids.push(operationId);
    pending.push(table.submit(() => operationId));
  }
  assert.equal(table.pendingCount, ids.length);
  for (let index = ids.length - 1; index >= 0; index--) {
    const record = terminal(ids[index]);
    const bytes = Buffer.alloc(4);
    bytes.writeUInt32LE(index);
    record.parts = [ZLinkBufferMessage.fromOwned(bytes)];
    table.complete(record);
  }
  const completed = await Promise.all(pending);
  for (let index = 0; index < ids.length; index++)
    assert.equal(completed[index].parts[0].data().readUInt32LE(), index);
  assert.equal(table.pendingCount, 0);
  table.dispose();
});
