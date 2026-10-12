const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate } = require('node:timers/promises');
const framework = require('../../packages/framework/dist/internal');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');

function fixture() {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n)
  );
  const raw = new RawServiceMeshRuntime({
    descriptor: {
      meshName: 'capacity',
      nodeRoutingId: 'local',
      lifecycleGeneration: 1n,
      descriptorRevision: 1n,
      advertisedEndpoint: 'inproc://capacity',
      channels: [],
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
  const runtime = new ServiceStatefulRuntime(raw, 'local', 1n);
  const actor = runtime.createActor('actor').ref;
  const spot = runtime.entrySpot();
  const payload = {
    packetName: 'Request',
    contentType: 'application/json',
    payload: Buffer.from('{}')
  };
  return {
    queue,
    raw,
    runtime,
    request(kind) {
      return kind === 'spot'
        ? runtime.requestToSpot(
            'local',
            {
              targetNodeRid: 'local',
              targetNodeGeneration: 1n,
              spot: spot.ref,
              authorityOwnerGeneration: spot.authorityOwnerGeneration,
              ownerLeaseGeneration: 1n
            },
            payload,
            1000
          )
        : runtime.requestToActor(actor, 1n, 1n, payload, 1000);
    }
  };
}

for (const kind of ['spot', 'actor']) {
  test(`local ${kind} public admission ends at mailbox acceptance before reply`, async () => {
    const f = fixture();
    let operation;
    const registration = framework.createFrameworkRegistration({
      spotNodes: { capacity: { router: { bind: 'inproc://local-admission' } } }
    });
    const client = new framework.DefaultZLinkRouteClient(registration, {
      request() {
        operation = f.request(kind);
        return operation.promise;
      }
    });
    const occupied = await f.queue.acquire(undefined, 'remote');
    try {
      const pending = client.requestToNode('capacity', 'local', {}).submit();
      assert.ok(pending instanceof Promise);
      assert.ok(pending.admitted instanceof Promise);
      let admitted = false;
      let replied = false;
      void pending.admitted.then(() => { admitted = true; });
      void pending.then(() => { replied = true; }, () => undefined);
      await setImmediate();
      assert.equal(admitted, false);
      assert.equal(replied, false);
      occupied.releaseAfterInternalProcessing();
      await pending.admitted;
      assert.equal(replied, false);
      const terminal = assert.rejects(pending, { name: 'OperationCancelledError' });
      f.runtime.operations.cancel(operation.id);
      await terminal;
      assert.equal(admitted, true);
    } finally {
      occupied.releaseAfterInternalProcessing();
      f.runtime.close();
      f.raw.close();
    }
  });

  for (const terminal of ['deadline', 'cancel', 'shutdown']) {
    test(`local ${kind} request removes capacity waiter on ${terminal}`, async () => {
      const f = fixture();
      const occupied = await f.queue.acquire(undefined, 'remote');
      try {
        const pending = f.request(kind);
        const rejected = assert.rejects(pending.promise, {
          name: terminal === 'deadline' ? 'OperationTimeoutError' : 'OperationCancelledError'
        });
        assert.equal(f.queue.snapshot().capacityWaiters, 1n);
        if (terminal === 'deadline') {
          assert.equal(f.runtime.expireOperations(performance.now() + 1001, Infinity), 1);
        } else if (terminal === 'cancel') {
          assert.equal(f.runtime.operations.cancel(pending.id), true);
        } else {
          f.runtime.close();
        }
        await rejected;
        await setImmediate();
        assert.equal(f.runtime.pendingOperationCount, 0);
        assert.equal(f.queue.snapshot().capacityWaiters, 0n);
        assert.equal(f.queue.snapshot().permitsInUse, 1n);
      } finally {
        f.runtime.close();
        f.raw.close();
        occupied.releaseAfterInternalProcessing();
      }
      assert.equal(f.queue.snapshot().permitsInUse, 0n);
    });
  }

  test(`local ${kind} request returns raced permit once without publishing after terminal`, async () => {
    const f = fixture();
    const occupied = await f.queue.acquire(undefined, 'remote');
    let published = 0;
    let returned = 0;
    const reserve = f.raw.reserveLocalIngress.bind(f.raw);
    f.raw.reserveLocalIngress = async (signal) => {
      const owner = await reserve(signal);
      const close = owner.close.bind(owner);
      owner.close = () => {
        returned++;
        close();
      };
      return owner;
    };
    f.runtime.submitLocalRequest = async () => {
      published++;
    };
    try {
      const pending = f.request(kind);
      const rejected = assert.rejects(pending.promise, { name: 'OperationCancelledError' });
      occupied.releaseAfterInternalProcessing();
      f.runtime.operations.cancel(pending.id);
      await rejected;
      await setImmediate();
      assert.equal(returned, 1);
      assert.equal(published, 0);
      assert.equal(f.queue.snapshot().capacityWaiters, 0n);
      assert.equal(f.queue.snapshot().permitsInUse, 0n);
    } finally {
      f.runtime.close();
      f.raw.close();
    }
  });
}
