const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate } = require('node:timers/promises');
const { runWithApplicationJobPermit, releaseApplicationJobPermitBeforeHandler } =
  require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
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
} = require('../../packages/framework/dist/internal');
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
    request(kind, onAdmitted) {
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
            1000,
            onAdmitted
          )
        : runtime.requestToActor(actor, payload, 1000, undefined, undefined, onAdmitted);
    }
  };
}

for (const kind of ['spot', 'actor']) {
  for (const terminal of ['cancel', 'deadline']) {
    test(`accepted local ${kind} reacquires admission after ${terminal} before running`, async () => {
      const f = fixture();
      const serial = kind === 'spot'
        ? new framework.ZLinkSpotSerialTurnExecutor()
        : new framework.ZLinkActorSerialExecutor('actor', 'spot');
      let finish;
      let started;
      const entered = new Promise(resolve => { started = resolve; });
      const running = serial.execute(async () => {
        started();
        await new Promise(resolve => { finish = resolve; });
      });
      await entered;
      const calls = [];
      let accepted;
      let occupied;
      // Hold only target execution; admission and caller completion use the runtime owners.
      f.runtime.submitLocalRequest = async ingress => {
        const lease = ingress.applicationJobOwner.takeInitial('application');
        lease.detachForHandlerTurn();
        try {
          accepted = runWithApplicationJobPermit(lease, () => serial.execute(() => {
            releaseApplicationJobPermitBeforeHandler();
            calls.push('accepted');
          }));
          ingress.onAdmitted?.();
          await accepted;
        } finally {
          lease.close();
        }
      };
      try {
        const pending = f.request(kind);
        let atPublication;
        pending.mapCaller(value => value, error => {
          atPublication = f.queue.snapshot();
          return error;
        });
        const rejected = assert.rejects(pending.promise);
        await setImmediate();
        assert.ok(accepted);
        assert.equal(f.queue.snapshot().permitsInUse, 1n);
        if (terminal === 'cancel') f.runtime.operations.cancel(pending.id);
        else f.runtime.expireOperations(performance.now() + 1001, Infinity);
        await rejected;
        assert.equal(atPublication.permitsInUse, 0n);
        assert.equal(atPublication.capacityWaiters, 0n);
        occupied = await f.queue.acquire(undefined, 'remote');
        const following = serial.execute(() => calls.push('following'));
        finish();
        await running;
        await setImmediate();
        assert.deepEqual(calls, []);
        assert.equal(f.queue.snapshot().capacityWaiters, 1n);
        occupied.releaseAfterInternalProcessing();
        await Promise.all([accepted, following]);
        assert.deepEqual(calls, ['accepted', 'following']);
        assert.equal(f.queue.snapshot().permitsInUse, 0n);
        assert.equal(f.queue.snapshot().capacityWaiters, 0n);
      } finally {
        occupied?.releaseAfterInternalProcessing();
        finish();
        await running;
        await accepted;
        f.runtime.close();
        f.raw.close();
        if (kind === 'spot') {
          serial.closeAdmission();
          await serial.whenIdle();
        } else await serial.close();
      }
    });
  }

  test(`local ${kind} public admission ends at mailbox acceptance before reply`, async () => {
    const f = fixture();
    let operation;
    const serial = new framework.ZLinkSpotSerialTurnExecutor();
    const outbound = new framework.DefaultZLinkSpotOutbound({
      serial,
      addressTransport: {
        requestToSpotAddress(_spot, _request, options) {
          operation = f.request(kind, options.onAdmitted);
          return operation.promise;
        }
      }
    });
    const occupied = await f.queue.acquire(undefined, 'remote');
    try {
      class Query {}
      const actorClient = new framework.DefaultZLinkActorClient({
        nodeProvider: () => ({
          requestToActor(_actor, _parts, options) {
            operation = f.request(kind, options.onAdmitted);
            return operation.id;
          }
        }),
        completionTableProvider: () => ({
          submit(start, _signal, requestOperation, receiver) {
            start();
            return deliverFixtureCompletion(operation.promise, requestOperation, receiver);
          }
        }),
        locationResolver: () => ({
          resolveDirectActorRoute: async () => ({
            kind: 'ready',
            route: {
              meshName: 'capacity',
              actorId: 'actor',
              actorRef: {
                meshName: 'capacity',
                actorId: 'actor',
                nodeRid: 'local',
                objectGeneration: 1n
              },
              ownerNodeRid: 'local',
              ownerNodeGeneration: 1n,
              spotKind: framework.ZLinkSpotKind.Entry,
              spotId: 'entry',
              spotGeneration: 1n,
              authorityOwnerGeneration: 1n,
              ownerId: 'local-owner',
              ownerLeaseGeneration: 1n
            }
          })
        })
      });
      let pending;
      await serial.execute(() => {
        pending = kind === 'actor'
          ? actorClient.requestToActor('actor', new Query()).submit()
          : outbound.requestToSpot('entry', new Query()).submit();
      });
      assert.ok(pending instanceof Promise);
      assert.ok(pending.admitted instanceof Promise);
      let admitted = false;
      let replied = false;
      void pending.admitted.then(() => {
        admitted = true;
      });
      void pending.then(
        () => {
          replied = true;
        },
        () => undefined
      );
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
        let atPublication;
        pending.mapCaller(value => value, error => {
          atPublication = f.queue.snapshot();
          return error;
        });
        const rejected = assert.rejects(pending.promise, terminal === 'deadline'
          ? error => error instanceof framework.ZLinkFrameworkException &&
            error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
          : { name: 'OperationCancelledError' });
        assert.equal(f.queue.snapshot().capacityWaiters, 1n);
        if (terminal === 'deadline') {
          assert.equal(f.runtime.expireOperations(performance.now() + 1001, Infinity), 1);
        } else if (terminal === 'cancel') {
          assert.equal(f.runtime.operations.cancel(pending.id), true);
        } else {
          f.runtime.close();
        }
        await rejected;
        assert.equal(atPublication.capacityWaiters, 0n);
        assert.equal(atPublication.permitsInUse, 1n);
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

  for (const terminal of ['deadline', 'cancel', 'shutdown']) {
    test(`queued local ${kind} returns its permit before ${terminal} publication`, async () => {
      const f = fixture();
      try {
        const pending = f.request(kind);
        let atPublication;
        pending.mapCaller(value => value, error => {
          atPublication = f.queue.snapshot();
          return error;
        });
        const rejected = assert.rejects(pending.promise);
        await setImmediate();
        assert.equal(f.queue.snapshot().permitsInUse, 1n);
        if (terminal === 'deadline') f.runtime.expireOperations(performance.now() + 1001, Infinity);
        else if (terminal === 'cancel') f.runtime.operations.cancel(pending.id);
        else f.runtime.close();
        await rejected;
        assert.equal(atPublication.permitsInUse, 0n);
        assert.equal(atPublication.capacityWaiters, 0n);
      } finally {
        f.runtime.close();
        f.raw.close();
      }
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

for (const waiting of [false, true]) {
  for (const terminal of ['cancel', 'deadline']) {
    test(`raw local request cleans admission before ${terminal} (waiting=${waiting})`, async () => {
      const f = fixture();
      const occupied = waiting ? await f.queue.acquire(undefined, 'remote') : undefined;
      const stop = new AbortController();
      try {
        const pending = f.raw.requestToNode('local', {
          packetName: 'Request', contentType: 'application/json', payload: Buffer.from('{}')
        }, 1000);
        pending.setSignal(stop.signal);
        let atPublication;
        pending.mapCaller(value => value, error => {
          atPublication = f.queue.snapshot();
          return error;
        });
        const rejected = assert.rejects(pending.promise);
        if (!waiting) await setImmediate();
        assert.equal(f.queue.snapshot().capacityWaiters, waiting ? 1n : 0n);
        if (terminal === 'cancel') stop.abort(new Error('caller cancelled'));
        else f.raw.expireOperations(performance.now() + 1001, Infinity);
        await rejected;
        assert.equal(atPublication.capacityWaiters, 0n);
        assert.equal(atPublication.permitsInUse, waiting ? 1n : 0n);
      } finally {
        occupied?.releaseAfterInternalProcessing();
        f.runtime.close();
        f.raw.close();
      }
    });
  }
}

function deliverFixtureCompletion(work, operation, receiver) {
  if (receiver === undefined) return work;
  void work.then(
    value => receiver.operationCompleted(operation.id, value, undefined),
    error => receiver.operationFailed(operation.id, error, undefined)
  );
}
