const assert = require('node:assert/strict');
const { Session } = require('node:inspector');
const { ZLinkMeshDispatchPump } = require('../../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const { RawServiceMeshRuntime } = require('../../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { ReadyDomain } = require('../../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const wire = require('../../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

function receiveFixture() {
  const runtime = Object.create(RawServiceMeshRuntime.prototype);
  const counts = { records: 0, iterators: 0, bytes: 0, closes: 0, releases: 0 };
  const parts = [Buffer.alloc(32), Buffer.alloc(1024)];
  const iterator = parts[Symbol.iterator];
  parts[Symbol.iterator] = function () {
    counts.iterators += 1;
    return iterator.call(this);
  };
  const record = { sourceRid: 'source', parts, close() { counts.closes += 1; } };
  const permit = { origin: 'remote', releaseAfterInternalProcessing() { counts.releases += 1; } };
  const router = { receive() { counts.records += 1; return record; } };
  runtime.requireStarted = () => router;
  runtime.observeInfrastructureFailure = () => {};
  runtime.processReceived = () => 'application';
  runtime.applicationJobQueue = {};
  const rotation = { take() { return permit; } };
  const observe = (_source, bytes) => { counts.bytes += bytes; };
  return { runtime, counts, record, permit, router, rotation, observe };
}

function claimWorkload() {
  const counts = { dispatch: 0, terminal: 0, release: 0, permitRelease: 0, close: 0 };
  const owner = { ordinaryIngressPreAdmitted: true };
  const permit = { releaseAfterInternalProcessing() { counts.permitRelease += 1; } };
  const record = {
    parts: [], applicationJobPermit: permit,
    onTerminalCompletion() { counts.terminal += 1; }
  };
  const received = { ok: true, records: [record] };
  const drained = { ok: true, hasResidue: false, records: [owner] };
  let remaining;
  const claim = {
    recvBatch() { return received; },
    release() {
      counts.release += 1;
      remaining -= 1;
      if (remaining === 0) pump.disposed = true;
    }
  };
  const batch = { reset() {}, close() { counts.close += 1; }, takeClaim() { return claim; } };
  const pump = new ZLinkMeshDispatchPump({
    createReadyBatch(capacity) { assert.equal(capacity, 1); return batch; },
    createReceiveBatch() { return batch; },
    drainReady() { return drained; }
  }, {
    applicationJobQueue: { acquire() { assert.fail('pre-admitted ingress must reuse its permit'); } },
    monotonicNowMs: () => 0,
    dispatch() { counts.dispatch += 1; },
    reportError(error) { throw error; }
  });
  return {
    counts, names: [],
    async run() {
      remaining = 1000;
      pump.disposed = false;
      pump.pendingDomains = ReadyDomain.Application;
      // Older revisions put the persistent worker in drainClaim itself.
      if (pump.runApplicationWorker) await pump.runApplicationWorker();
      else await pump.drainClaim(ReadyDomain.Application);
      assert.equal(remaining, 0);
    }
  };
}

function workload(mode) {
  if (mode === 'claim') return claimWorkload();
  const counts = { calls: 0 };
  if (mode === 'pump') {
    const pump = new ZLinkMeshDispatchPump({}, {});
    pump.wakeApplicationWorker = () => { counts.calls += 1; };
    const record = { parts: [], onTerminalCompletion() { counts.terminals += 1; } };
    counts.terminals = 0;
    return {
      counts,
      names: ['markReady', 'finishRecord'],
      run() {
        pump.pendingDomains = ReadyDomain.None;
        pump.markReady(ReadyDomain.Application);
        pump.finishRecord(record, undefined);
      }
    };
  }
  if (mode === 'receive') {
    const f = receiveFixture();
    return {
      counts: f.counts,
      names: ['receiveOne'],
      run() { assert.equal(f.runtime.receiveOne(0, f.observe, f.rotation), 'application'); }
    };
  }
  const runtime = Object.create(RawServiceMeshRuntime.prototype);
  if (mode === 'route') {
    runtime.observeInfrastructureFailure = () => {};
    runtime.receiveBatch = (ready) => { counts.calls += 1; return ready; };
    return { counts, names: ['pumpBatch'], run() { assert.equal(runtime.pumpBatch(true, false), true); } };
  }
  assert.equal(mode, 'dispatch');
  const record = {
    sourceRid: 'source', routeGeneration: 1,
    parts: [wire.encodeNodeRequestHeader(1n), Buffer.alloc(1024)]
  };
  runtime.topology = { admittedPeer() {
    return { liveness: { connectionId: 1 }, descriptor: { lifecycleGeneration: 1n } };
  } };
  runtime.liveness = { recordReceived() {} };
  runtime.nodeMailboxOwner = 'node';
  runtime.enqueueOrdinaryRecord = () => { counts.calls += 1; return 'application'; };
  const lease = {};
  const owner = { acquire() { return lease; } };
  return {
    counts, names: ['processReceived', 'processApplicationReceived'],
    run() { assert.equal(runtime.processReceived(record, 0, owner), 'application'); }
  };
}

function allocationControl() { return () => undefined; }

async function trace(mode) {
  const work = workload(mode);
  // Disable optimizing compilation in the child so escape analysis cannot hide allocations.
  if (mode === 'claim') {
    await work.run();
    for (let index = 0; index < 10000; index += 1) allocationControl();
  } else for (let index = 0; index < 10000; index += 1) { work.run(); allocationControl(); }
  for (const name of Object.keys(work.counts)) work.counts[name] = 0;
  global.gc();
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
      if (mode !== 'claim') work.run();
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
  }
}

module.exports = { receiveFixture };
if (require.main === module) {
  trace(process.argv[2]).then((result) => console.log(JSON.stringify(result)), (error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
