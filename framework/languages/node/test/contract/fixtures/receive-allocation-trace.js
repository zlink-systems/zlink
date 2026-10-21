const assert = require('node:assert/strict');
const { Session } = require('node:inspector');
const { ZLinkMeshDispatchPump } = require('../../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const { RawServiceMeshRuntime } = require('../../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { ReadyDomain } = require('../../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const wire = require('../../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

function rawFixture(mode) {
  const counts = { records: 0, iterators: 0, bytes: 0, closes: 0, releases: 0, calls: 0 };
  const parts = mode === 'dispatch'
    ? [wire.encodeNodeRequestHeader(1n), Buffer.alloc(1024)]
    : [Buffer.alloc(32), Buffer.alloc(1024)];
  const iterator = parts[Symbol.iterator];
  parts[Symbol.iterator] = function () { counts.iterators += 1; return iterator.call(this); };
  const record = { sourceRid: 'source', routeGeneration: 1, parts, close() { counts.closes += 1; } };
  const permit = { origin: 'remote', markApplicationQueued() {},
    releaseAfterInternalProcessing() { counts.releases += 1; } };
  const descriptor = {
    meshName: 'allocation', nodeRoutingId: 'local', lifecycleGeneration: 1n,
    descriptorRevision: 1n, advertisedEndpoint: 'inproc://allocation', channels: [],
    state: 'serving', securityIdentity: 'default', applicationVersion: 1n,
    protocolCapabilities: [wire.M6A_SERVICE_WIRE_REQUIRED_CAPABILITY], objectRole: 'server',
    placementWeight: 100, activeCapacityLimit: 100, pendingCapacityLimit: 16,
    activeCapacityUsed: 0, pendingCapacityUsed: 0
  };
  const router = {
    setRoutingId() {}, bind() {}, localEndpoint() { return descriptor.advertisedEndpoint; },
    receive() {
      counts.calls += 1;
      if (mode === 'route') return undefined;
      counts.records += 1;
      return record;
    }, close() {}
  };
  const runtime = new RawServiceMeshRuntime({
    descriptor,
    applicationJobQueue: {
      acquireOrResume() { return permit; },
      createReceiveRotation() { return { reserve() {}, take() { return permit; }, releaseUnused() {} }; }
    },
    bindingPort: { createHost() { return { createRouter() { return router; }, close() {} }; } }
  });
  runtime.start();
  if (mode === 'dispatch') {
    runtime.topology.admittedPeer = () => ({
      liveness: { connectionId: 1 }, descriptor: { lifecycleGeneration: 1n }
    });
    runtime.liveness.recordReceived = () => {};
    runtime.mailbox.tryEnqueue = (entry) => { entry.applicationJob.close(); return true; };
  }
  const observe = (_source, bytes) => { counts.bytes += bytes; };
  return {
    counts,
    close() { runtime.close(); },
    names: mode === 'route' ? ['pumpBatch'] : mode === 'dispatch'
      ? ['processReceived', 'processApplicationReceived'] : ['receiveOne'],
    async run() {
      if (mode === 'route') assert.equal(runtime.pumpBatch(true, false), false);
      else assert.equal(await runtime.pumpOne(0, observe), mode === 'dispatch' ? 'application' : 'dropped');
    }
  };
}

function claimWorkload(mode) {
  const counts = mode === 'pump' ? { calls: 0, terminals: 0 }
    : { dispatch: 0, terminal: 0, release: 0, permitRelease: 0, close: 0 };
  const owner = { ordinaryIngressPreAdmitted: true };
  const permit = { releaseAfterInternalProcessing() { if (mode !== 'pump') counts.permitRelease += 1; } };
  const record = {
    parts: [], applicationJobPermit: permit,
    onTerminalCompletion() { if (mode === 'pump') counts.terminals += 1; else counts.terminal += 1; }
  };
  const received = { ok: true, records: [record] };
  const drained = { ok: true, hasResidue: false, records: [owner] };
  const empty = { ok: false, records: [] };
  return {
    counts, names: mode === 'pump' ? ['markReady', 'finishRecord'] : [],
    async run() {
      let ready;
      let remaining = mode === 'pump' ? 1 : 1000;
      let done;
      const finished = new Promise((resolve) => { done = resolve; });
      const claim = {
        recvBatch() { return received; },
        release() { if (mode !== 'pump') counts.release += 1; remaining -= 1; }
      };
      const batch = { reset() {}, close() { if (mode !== 'pump') counts.close += 1; }, takeClaim() { return claim; } };
      const pump = new ZLinkMeshDispatchPump({
        setReadyHandler(handler) { ready = handler; },
        createReadyBatch(capacity) { assert.equal(capacity, 1); return batch; },
        createReceiveBatch() { return batch; },
        drainReady() { if (remaining !== 0) return drained; done(); return empty; }
      }, {
        applicationJobQueue: { acquire() { assert.fail('pre-admitted ingress must reuse its permit'); } },
        monotonicNowMs: () => 0,
        dispatch() { if (mode === 'pump') counts.calls += 1; else counts.dispatch += 1; },
        reportError(error) { throw error; }
      });
      pump.start();
      try { ready(ReadyDomain.Application); await finished; }
      finally { await pump.dispose(); }
      assert.equal(remaining, 0);
    }
  };
}

function workload(mode) {
  return mode === 'claim' || mode === 'pump' ? claimWorkload(mode) : rawFixture(mode);
}

function allocationControl() { return () => undefined; }

async function trace(mode) {
  const work = workload(mode);
  // Disable optimizing compilation in the child so escape analysis cannot hide allocations.
  if (mode === 'claim') {
    await work.run();
    for (let index = 0; index < 10000; index += 1) allocationControl();
  } else for (let index = 0; index < 10000; index += 1) { await work.run(); allocationControl(); }
  global.gc();
  for (const name of Object.keys(work.counts)) work.counts[name] = 0;
  const session = new Session();
  session.connect();
  const post = (method, params = {}) => new Promise((resolve, reject) => {
    session.post(method, params, (error, result) => error ? reject(error) : resolve(result));
  });
  const chunks = [];
  session.on('HeapProfiler.addHeapSnapshotChunk', ({ params }) => chunks.push(params.chunk));
  try {
    // Allocation tracking counts every allocation, including objects collected before the snapshot.
    await post('HeapProfiler.startTrackingHeapObjects', { trackAllocations: true });
    if (mode === 'claim') await work.run();
    for (let index = 0; index < 1000; index += 1) {
      if (mode !== 'claim') await work.run();
      allocationControl();
    }
    await post('HeapProfiler.takeHeapSnapshot');
    const snapshot = JSON.parse(chunks.join(''));
    session.removeAllListeners('HeapProfiler.addHeapSnapshotChunk');
    await post('HeapProfiler.stopTrackingHeapObjects');
    const fields = snapshot.snapshot.meta.trace_function_info_fields;
    const infos = snapshot.trace_function_infos;
    const names = [];
    for (let index = 0; index < infos.length; index += fields.length) {
      names.push({
        name: snapshot.strings[infos[index + fields.indexOf('name')]],
        script: snapshot.strings[infos[index + fields.indexOf('script_name')]]
      });
    }
    const allocations = Object.fromEntries(work.names.map((name) => [name, 0]));
    let controlAllocations = 0;
    const moduleAllocations = { count: 0, bytes: 0 };
    const moduleFunctions = {};
    function visit(tree) {
      const traceFields = snapshot.snapshot.meta.trace_node_fields;
      for (let index = 0; index < tree.length; index += traceFields.length) {
        const info = names[tree[index + traceFields.indexOf('function_info_index')]];
        if (info.name === 'allocationControl' && info.script === __filename) {
          controlAllocations += tree[index + traceFields.indexOf('count')];
        }
        if (info.script.endsWith('/runtime/backend/mesh-dispatch-pump.js')) {
          const count = tree[index + traceFields.indexOf('count')];
          const bytes = tree[index + traceFields.indexOf('size')];
          moduleAllocations.count += count;
          moduleAllocations.bytes += bytes;
          moduleFunctions[info.name] = (moduleFunctions[info.name] ?? 0) + count;
        }
        if (Object.hasOwn(allocations, info.name) && info.script.includes('/packages/framework/dist/')) {
          allocations[info.name] += tree[index + traceFields.indexOf('count')];
        }
        visit(tree[index + traceFields.indexOf('children')]);
      }
    }
    visit(snapshot.trace_tree);
    assert.equal(controlAllocations, 1000, 'allocation tracker must count discarded closures');
    return { counts: work.counts, allocations, controlAllocations, moduleAllocations, moduleFunctions };
  } finally {
    session.disconnect();
    work.close?.();
  }
}

if (require.main === module) {
  trace(process.argv[2]).then((result) => console.log(JSON.stringify(result)), (error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
