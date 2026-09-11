'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } =
  require('../../packages/framework/dist/runtime/host/application-job-queue');
const { SERVICE_WIRE_REQUIRED_CAPABILITY } =
  require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

for (const [size, clockStep, expected] of [[1, 0, 64], [2 * 1024 * 1024, 0, 2], [1, 3, 1]]) {
  test(`raw ingress round bounds all peers: ${size} bytes, ${clockStep}ms per record`, async t => {
    let received = 0;
    let closed = 0;
    let monitorDrains = 0;
    const payload = Buffer.alloc(size); // malformed control still consumes ordinary ingress budget
    const router = {
      setRoutingId() {}, setReceiveFlowState() {}, bind() {}, close() {},
      localEndpoint: () => 'inproc://round',
      monitor: () => ({ drain() { monitorDrains++; return 0; }, close() {} }),
      receive() {
        const peer = `peer-${received++ % 2}`;
        return { sourceRid: peer, sourceRoute: Buffer.from(peer), parts: [payload], close() { closed++; } };
      }
    };
    const runtime = new RawServiceMeshRuntime({
      descriptor: {
        meshName: 'round', nodeRoutingId: 'round', lifecycleGeneration: 1n, descriptorRevision: 1n,
        advertisedEndpoint: 'inproc://round', channels: [], state: 'serving', securityIdentity: 'test',
        applicationVersion: 1n, protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'none',
        placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 10,
        activeCapacityUsed: 0, pendingCapacityUsed: 0
      },
      bindingPort: { createHost: () => ({ createRouter: () => router, close() {} }) },
      applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration())
    });
    runtime.start();
    monitorDrains = 0;
    t.mock.method(performance, 'now', () => received * clockStep);
    try {
      assert.equal(await runtime.pumpBatch(), true);
      assert.equal(received, expected);
      assert.equal(closed, expected);
      assert.equal(monitorDrains, 1, 'monitor work belongs to the round, not each record');
    } finally {
      runtime.close();
    }
  });
}
