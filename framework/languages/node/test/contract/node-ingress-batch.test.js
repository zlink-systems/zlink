'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { AsyncLocalStorage } = require('node:async_hooks');
const backend = require('../../packages/framework/dist/runtime/backend');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } =
  require('../../packages/framework/dist/runtime/host/application-job-queue');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

test('mesh owner drains 64 pre-admitted records in one receive batch without losing FIFO', async t => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}));
  const node = factory.createMeshAdapter().createMeshNode(context, {
    meshName: 'ingress-batch', routingId: `ingress-batch-${process.pid}`, applicationJobQueue: queue
  });
  const batches = [];
  const received = [];
  const createReadyBatch = node.createReadyBatch.bind(node);
  node.createReadyBatch = capacity => {
    const batch = createReadyBatch(capacity);
    const takeClaim = batch.takeClaim.bind(batch);
    batch.takeClaim = index => {
      const claim = takeClaim(index);
      const recvBatch = claim.recvBatch.bind(claim);
      claim.recvBatch = (...args) => {
        const result = recvBatch(...args);
        if (result.records.length) batches.push(result.records.length);
        return result;
      };
      return claim;
    };
    return batch;
  };
  let finish;
  let fail;
  const done = new Promise((resolve, reject) => { finish = resolve; fail = reject; });
  let pump;
  try {
    node.setBind(`inproc://ingress-batch-${process.pid}`);
    node.addChannelName('batch');
    node.start();
    for (let i = 0; i < 64; i++) {
      await node.sendToChannel('batch', Buffer.from(String(i)));
    }
    let areaEntries = 0;
    let ioTurn = false;
    let firstHandlerAfterIo;
    const run = AsyncLocalStorage.prototype.run;
    t.mock.method(AsyncLocalStorage.prototype, 'run', function (store, ...args) {
      if (store === 'application') areaEntries++;
      return run.call(this, store, ...args);
    });
    pump = new backend.ZLinkMeshDispatchPump(node, {
      applicationJobQueue: queue,
      monotonicNowMs: () => 0,
      dispatch(_owner, record) {
        firstHandlerAfterIo ??= ioTurn;
        received.push(Number(record.parts[0].data().toString()));
        if (received.length === 64) finish();
      },
      reportError: fail
    });
    setImmediate(() => { ioTurn = true; });
    pump.start();
    await done;
    assert.deepEqual(received, Array.from({ length: 64 }, (_, i) => i));
    assert.deepEqual(batches, [64]);
    assert.equal(areaEntries, 1, 'one execution-area scope spans the whole owner drain');
    assert.equal(firstHandlerAfterIo, false, 'ready work begins in a microtask');
  } finally {
    await pump?.dispose();
    node.close();
    await context.dispose();
  }
});

test('a failed handler releases every record retained by its receive batch', async () => {
  const closed = Array(64).fill(0);
  const released = Array(64).fill(0);
  const failure = new Error('handler failed');
  let report;
  const reported = new Promise(resolve => { report = resolve; });
  let ready;
  let claimed = false;
  let consumed = false;
  const claim = {
    recvBatch() {
      if (consumed) return { ok: false, records: [] };
      consumed = true;
      return { ok: true, records: closed.map((_, i) => ({
        parts: [{ data() { return Buffer.alloc(0); }, close() { closed[i]++; } }],
        applicationJobPermit: { releaseAfterInternalProcessing() {} },
        releaseRetainedIngress() { released[i]++; }
      })) };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { ready = handler; },
    createReadyBatch() { return { reset() {}, takeClaim() { return claim; }, close() {} }; },
    createReceiveBatch() { return { reset() {}, close() {} }; },
    drainReady() {
      if (claimed) return { ok: false, records: [] };
      claimed = true;
      return { ok: true, hasResidue: false, records: [{ ordinaryIngressPreAdmitted: true }] };
    }
  };
  let dispatches = 0;
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: { acquire() { throw new Error('already admitted'); } },
    dispatch() { dispatches++; throw failure; },
    reportError: report
  });
  try {
    pump.start();
    ready(ReadyDomain.Application);
    assert.equal(await reported, failure);
    assert.equal(dispatches, 1);
    assert.deepEqual(closed, Array(64).fill(1));
    assert.deepEqual(released, Array(64).fill(1));
  } finally {
    await pump.dispose();
  }
});
