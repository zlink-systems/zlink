const assert = require('node:assert/strict');
const test = require('node:test');
const { RequestResult, SubmitResult } = require('@zlink-systems/zlink');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY,
  ServiceWireFrameworkErrorCode
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

test('RouteMesh with only its own server returns target-not-found without local admission', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n)
  );
  const runtime = new RawServiceMeshRuntime({
    descriptor: {
      meshName: 'self',
      nodeRoutingId: 'local',
      lifecycleGeneration: 1n,
      descriptorRevision: 1n,
      advertisedEndpoint: 'inproc://self',
      channels: [{ name: 'orders', weight: 100 }],
      state: 'serving',
      securityIdentity: 'default',
      applicationVersion: 1n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
      objectRole: 'server',
      placementWeight: 100,
      activeCapacityLimit: 1,
      pendingCapacityLimit: 1,
      activeCapacityUsed: 0,
      pendingCapacityUsed: 0
    },
    applicationJobQueue: queue
  });
  try {
    const request = runtime.requestToChannel('orders', Buffer.from('{}'), 1000);
    // Selection must terminate before touching the host Application Job Queue.
    assert.equal(queue.snapshot().permitsInUse, 0n);
    assert.deepEqual(await request.promise, {
      terminalResult: RequestResult.NotFound,
      failureCode: ServiceWireFrameworkErrorCode.requestTargetNotFound
    });
    assert.equal(await runtime.sendToChannel('orders', Buffer.from('{}')), SubmitResult.NotFound);
    assert.equal(queue.snapshot().capacityWaitCount, 0n);
    assert.equal(runtime.mailbox.pendingMessages('application'), 0);
  } finally {
    runtime.close();
  }
});
