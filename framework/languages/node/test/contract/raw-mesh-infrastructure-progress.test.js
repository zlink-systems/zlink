const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ZLinkRuntimeTaskRunner,
  ZLinkRuntimeTaskErrorSink
} = require('../../packages/framework/dist/runtime/execution');
const {
  ZLinkBackendResultError,
  SubmitResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');

function harness(options = {}) {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n })
  );
  const records = [];
  const sent = [];
  let send = async () => {};
  let observations = 0;
  const descriptor = {
    meshName: 'infra-progress',
    nodeRoutingId: 'local',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: 'inproc://infra-progress',
    channels: [],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 16,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    setReadableHandler() {},
    bind() {},
    localEndpoint: () => descriptor.advertisedEndpoint,
    routesSnapshot() {
      observations++;
      return [];
    },
    send(target, parts) {
      sent.push([target, parts]);
      return send();
    },
    submitSend(target, parts) {
      sent.push([target, parts]);
      return { result: SubmitResult.Backpressured, admitted: send() };
    },
    receive: () => records.shift(),
    close() {}
  };
  let resumes = 0;
  const runtime = new RawServiceMeshRuntime({
    ...options,
    descriptor,
    applicationJobQueue: queue,
    onReceiveReady: () => resumes++,
    bindingPort: { createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} }) }
  });
  runtime.start();
  return {
    runtime,
    queue,
    records,
    sent,
    setSend: (value) => {
      send = value;
    },
    observations: () => observations,
    resumes: () => resumes
  };
}

test('ordinary capacity ends only the receive turn while route observation and liveness continue', async () => {
  const h = harness();
  const held = await h.queue.acquire(undefined, 'remote');
  let ticks = 0;
  h.runtime.liveness.tick = () => {
    ticks++;
    return { probes: [], timedOutNodes: [] };
  };
  let completed = false;
  const round = h.runtime.pumpBatch(true, true).then(() => {
    completed = true;
  });
  try {
    await turn();
    assert.equal(completed, true);
    assert.equal(ticks, 1);
    assert.equal(h.observations(), 1);
    await h.runtime.pumpBatch(true, true);
    assert.equal(h.queue.snapshot().capacityWaitCount, 1n);
    assert.equal(ticks, 2);
    held.releaseAfterInternalProcessing();
    await turn();
    assert.equal(h.resumes(), 1);
    await h.runtime.pumpBatch(true);
    assert.equal(h.queue.snapshot().permitsInUse, 0n);
  } finally {
    held.releaseAfterInternalProcessing();
    h.runtime.close();
    await round;
  }
});

test('backpressured control submission stays submitted when route absence settles early or late', async () => {
  for (const late of [false, true]) {
    const h = harness();
    let reject;
    const cause = new ZLinkBackendResultError('submit', SubmitResult.NotConnected, undefined, {
      phase: 'completion'
    });
    h.setSend(() =>
      late
        ? new Promise((_resolve, fail) => {
            reject = fail;
          })
        : Promise.reject(cause)
    );
    try {
      const result = await h.runtime.sendControl('peer', [wire.encodeReject(1)]);
      assert.equal(result, true, `Backpressured must be submitted (late=${late})`);
      reject?.(cause);
      await turn();
    } finally {
      reject?.(cause);
      h.runtime.close();
    }
  }
});

test('queue grants synchronously or resumes its oldest waiter, without promise timing', async () => {
  const h = harness();
  try {
    const resumes = [];
    const held = h.queue.acquireOrResume((permit) => resumes.push(permit), assert.fail,
      undefined,
      'remote');
    assert.ok(held);
    assert.equal(
      h.queue.acquireOrResume((permit) => resumes.push(permit), assert.fail, undefined, 'remote'),
      undefined
    );
    assert.equal(resumes.length, 0);
    held.releaseAfterInternalProcessing();
    await turn();
    assert.equal(resumes.length, 1);
    resumes[0].releaseAfterInternalProcessing();
    assert.equal(h.queue.snapshot().permitsInUse, 0n);
  } finally {
    h.runtime.close();
  }
});

test('late control SEND faults reach the existing infrastructure task owner', async () => {
  const errorSink = new ZLinkRuntimeTaskErrorSink();
  const failures = [];
  errorSink.onRuntimeTaskException((failure) => failures.push(failure));
  const h = harness({
    infrastructureTaskRunner: new ZLinkRuntimeTaskRunner(errorSink, new AbortController().signal)
  });
  let reject;
  h.setSend(
    () =>
      new Promise((_resolve, fail) => {
        reject = fail;
      })
  );
  h.runtime.liveness.tick = () => ({
    probes: [{ nodeRoutingId: 'peer', probeId: 1n }],
    timedOutNodes: []
  });
  const cause = new Error('control admission failed');
  try {
    await h.runtime.pumpBatch(false);
    reject(cause);
    await turn();
    assert.equal(failures.length, 1);
    assert.equal(failures[0].error, cause);
  } finally {
    h.runtime.close();
  }
});

test('closing the receive owner cancels its one FIFO acquisition without retaining a grant', async () => {
  const h = harness();
  const held = await h.queue.acquire(undefined, 'remote');
  await h.runtime.pumpBatch(true);
  assert.equal(h.queue.snapshot().capacityWaiters, 1n);
  h.runtime.close();
  held.releaseAfterInternalProcessing();
  await turn();
  assert.equal(h.queue.snapshot().capacityWaiters, 0n);
  assert.equal(h.queue.snapshot().permitsInUse, 0n);
});

test('pending reverse liveness SEND leaves another record receivable and bounds duplicate controls', async () => {
  const h = harness();
  let release;
  h.setSend(
    () =>
      new Promise((resolve) => {
        release = resolve;
      })
  );
  let probe = 0n;
  h.runtime.liveness.tick = () => ({
    probes: [{ nodeRoutingId: 'peer', probeId: ++probe }],
    timedOutNodes: []
  });
  let completed = false;
  const round = h.runtime.pumpBatch(false).then(() => {
    completed = true;
  });
  try {
    await turn();
    assert.equal(completed, true);
    h.records.push({
      sourceRid: 'other-peer',
      parts: [Buffer.from(wire.encodeReject(1))],
      close() {}
    });
    let observed = 0;
    const result = await h.runtime.pumpOne(performance.now(), () => {
      observed++;
    });
    assert.equal(observed, 1);
    assert.equal(result, 'infrastructure');
    for (let i = 0; i < 8; i++) await h.runtime.pumpBatch(false);
    assert.equal(h.sent.length, 1);
    h.setSend(async () => {});
    release();
    await turn();
    assert.equal(h.sent.length, 2);
  } finally {
    h.setSend(async () => {});
    release?.();
    h.runtime.close();
    await round;
  }
});
