const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

test('one request receive round evaluates permit pressure no more than the pre-rotation path', async (t) => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 256n }, () => 1n)
  );
  const runtime = new RawServiceMeshRuntime({
    applicationJobQueue: queue,
    descriptor: {
      meshName: 'mesh',
      nodeRoutingId: 'target',
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
  let received = false;
  runtime.router = {
    receive() {
      if (received) return undefined;
      received = true;
      return { sourceRid: 'source', parts: [Buffer.alloc(0)], close() {} };
    }
  };
  runtime.processReceived = (_record, _now, owner) => {
    const job = owner.takeInitial('application');
    job.releaseBeforeHandler();
    job.close();
    return 'application';
  };
  runtime.tickLiveness = () => {};
  let pressureEvaluations = 0;
  const evaluate = queue.evaluatePressure;
  queue.evaluatePressure = function () {
    pressureEvaluations++;
    return evaluate.call(this);
  };
  await runtime.pumpBatch();
  assert.equal(received, true);
  assert.equal(queue.snapshot().permitsInUse, 0n);
  t.diagnostic(`pressure evaluations per request: ${pressureEvaluations}`);
  // Before receive rotation: reserve/release for the request and the no-data probe.
  assert.ok(
    pressureEvaluations <= 4,
    `pressure evaluations per request: ${pressureEvaluations} > 4`
  );
  runtime.mailbox.close();
});

test('unused rotation reservations return capacity to FIFO waiters and preserve retained storage', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 4n }, () => 1n)
  );
  const rotation = queue.createReceiveRotation(4, 'remote');
  rotation.reserve();
  const retained = rotation.take();
  const order = [];
  const first = queue.acquire(undefined, 'local').then((permit) => {
    order.push(1);
    return permit;
  });
  const second = queue.acquire(undefined, 'local').then((permit) => {
    order.push(2);
    return permit;
  });
  rotation.releaseUnused();
  const leases = await Promise.all([first, second]);
  assert.deepEqual(order, [1, 2]);
  assert.equal(queue.snapshot().permitsInUse, 3n);
  for (const lease of leases) lease.releaseAfterInternalProcessing();
  rotation.reserve();
  const next = rotation.take();
  assert.notEqual(next, retained);
  next.releaseAfterInternalProcessing();
  next.releaseStorage();
  rotation.releaseUnused();
  retained.releaseAfterInternalProcessing();
  retained.releaseStorage();
  assert.equal(queue.snapshot().permitsInUse, 0n);
});
