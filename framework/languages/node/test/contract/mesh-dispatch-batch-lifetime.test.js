'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const backend = require('../../packages/framework/dist/runtime/backend');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } =
  require('../../packages/framework/dist/runtime/host/application-job-queue');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

function applicationQueue(limit = 1n) {
  return new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({
    maxQueuedApplicationJobs: limit
  }));
}

test('application worker reuses its ready and receive batches across sparse readiness', async () => {
  let ready;
  let pending;
  let dispatched;
  const readyBatches = [];
  const receiveBatches = [];
  const errors = [];
  const node = {
    setReadyHandler(handler) { ready = handler; },
    createReadyBatch() {
      const batch = {
        closes: 0,
        reset() {},
        takeClaim() {
          const record = pending;
          pending = undefined;
          let consumed = false;
          return {
            recvBatch() {
              if (consumed) return { ok: false, records: [] };
              consumed = true;
              return { ok: true, records: [record] };
            },
            release() {}
          };
        },
        close() { this.closes++; }
      };
      readyBatches.push(batch);
      return batch;
    },
    createReceiveBatch(capacity) {
      const batch = { capacity, closes: 0, reset() {}, close() { this.closes++; } };
      receiveBatches.push(batch);
      return batch;
    },
    drainReady() {
      return pending === undefined
        ? { ok: false, hasResidue: false, records: [] }
        : { ok: true, hasResidue: false, records: [{ ordinaryIngressPreAdmitted: true }] };
    }
  };
  const queue = applicationQueue();
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: queue,
    monotonicNowMs: () => 0,
    dispatch(_owner, record) { dispatched.resolve(record.sequence); },
    reportError(error) { errors.push(error); }
  });
  try {
    pump.start();
    for (let sequence = 0; sequence < 8; sequence++) {
      dispatched = deferred();
      const permit = await queue.acquire();
      permit.markApplicationQueued();
      pending = { sequence, parts: [], applicationJobPermit: permit };
      ready(ReadyDomain.Application);
      assert.equal(await dispatched.promise, sequence);
      await new Promise(resolve => setImmediate(resolve));
    }
    assert.equal(readyBatches.length, 1, 'one ready batch belongs to the persistent worker');
    assert.equal(receiveBatches.length, 1, 'the 64-record receive batch survives idle waits');
    assert.equal(receiveBatches[0].capacity, 64);
    assert.equal(readyBatches[0].closes, 0);
    assert.equal(receiveBatches[0].closes, 0);
    assert.equal(queue.snapshot().permitsInUse, 0n);
    assert.deepEqual(errors, []);
  } finally {
    await pump.dispose();
  }
  assert.ok(readyBatches.every(batch => batch.closes === 1));
  assert.ok(receiveBatches.every(batch => batch.closes === 1));
});

test('a receive exception returns its untransferred application permit exactly once', async () => {
  let ready;
  let claimed = false;
  let released = 0;
  let claimsReleased = 0;
  const failure = new Error('receive failed');
  const reported = deferred();
  const queue = applicationQueue();
  const acquire = queue.acquire.bind(queue);
  queue.acquire = async signal => {
    const permit = await acquire(signal);
    const release = permit.releaseAfterInternalProcessing.bind(permit);
    permit.releaseAfterInternalProcessing = () => { released++; release(); };
    return permit;
  };
  const node = {
    setReadyHandler(handler) { ready = handler; },
    createReadyBatch() {
      return {
        reset() {}, close() {},
        takeClaim() {
          return {
            recvBatch() { throw failure; },
            release() { claimsReleased++; }
          };
        }
      };
    },
    createReceiveBatch() { return { reset() {}, close() {} }; },
    drainReady() {
      if (claimed) return { ok: false, hasResidue: false, records: [] };
      claimed = true;
      return { ok: true, hasResidue: false, records: [{}] };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: queue,
    dispatch() { assert.fail('a failed receive cannot dispatch'); },
    reportError(error) { reported.resolve(error); }
  });
  try {
    pump.start();
    ready(ReadyDomain.Application);
    assert.equal(await reported.promise, failure);
    assert.equal(queue.snapshot().permitsInUse, 0n);
    assert.equal(released, 1);
    assert.equal(claimsReleased, 1);
  } finally {
    await pump.dispose();
  }
});

test('shutdown during permit handoff returns the reservation before any receive', async () => {
  let ready;
  let receives = 0;
  const waiting = deferred();
  const handoff = deferred();
  const queue = applicationQueue();
  const permit = await queue.acquire();
  const node = {
    setReadyHandler(handler) { ready = handler; },
    createReadyBatch() {
      return {
        reset() {}, close() {},
        takeClaim() {
          return {
            recvBatch() { receives++; return { ok: false, records: [] }; },
            release() {}
          };
        }
      };
    },
    createReceiveBatch() { return { reset() {}, close() {} }; },
    drainReady() { return { ok: true, hasResidue: false, records: [{}] }; }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: { acquire() { waiting.resolve(); return handoff.promise; } },
    dispatch() { assert.fail('shutdown cannot dispatch'); }
  });
  pump.start();
  ready(ReadyDomain.Application);
  await waiting.promise;
  const stopped = pump.dispose();
  handoff.resolve(permit);
  await stopped;
  assert.equal(receives, 0);
  assert.equal(queue.snapshot().permitsInUse, 0n);
});

for (const result of ['no-data', 'empty-batch', 'dispatch', 'dispatch-failure']) {
  test(`claim reservation has one terminal owner after ${result}`, async () => {
    let ready;
    let claimed = false;
    let receives = 0;
    let dispatches = 0;
    const released = deferred();
    const queue = applicationQueue();
    const reservations = [];
    const failure = new Error('dispatch failed');
    const errors = [];
    const acquire = queue.acquire.bind(queue);
    queue.acquire = async signal => {
      const permit = await acquire(signal);
      const reservation = { queued: 0, releases: 0 };
      reservations.push(reservation);
      const mark = permit.markApplicationQueued.bind(permit);
      const release = permit.releaseAfterInternalProcessing.bind(permit);
      permit.markApplicationQueued = () => { reservation.queued++; mark(); };
      permit.releaseAfterInternalProcessing = () => { reservation.releases++; release(); };
      return permit;
    };
    const node = {
      setReadyHandler(handler) { ready = handler; },
      createReadyBatch() {
        return {
          reset() {}, close() {},
          takeClaim() {
            return {
              recvBatch() {
                if (++receives > 1 || result === 'no-data') return { ok: false, records: [] };
                return { ok: true, records: result === 'empty-batch' ? [] : [{ parts: [] }] };
              },
              release() { released.resolve(); }
            };
          }
        };
      },
      createReceiveBatch() { return { reset() {}, close() {} }; },
      drainReady() {
        if (claimed) return { ok: false, hasResidue: false, records: [] };
        claimed = true;
        return { ok: true, hasResidue: false, records: [{}] };
      }
    };
    const pump = new backend.ZLinkMeshDispatchPump(node, {
      applicationJobQueue: queue,
      dispatch() {
        dispatches++;
        if (result === 'dispatch-failure') throw failure;
      },
      reportError(error) { errors.push(error); }
    });
    pump.start();
    ready(ReadyDomain.Application);
    await released.promise;
    await pump.dispose();
    assert.equal(dispatches, result.startsWith('dispatch') ? 1 : 0);
    assert.ok(reservations.every(reservation => reservation.releases === 1));
    assert.equal(reservations.reduce((sum, reservation) => sum + reservation.queued, 0), dispatches);
    assert.equal(queue.snapshot().permitsInUse, 0n);
    assert.deepEqual(errors, result === 'dispatch-failure' ? [failure] : []);
  });
}

for (const partsPerRecord of [1, 2]) {
  test(`raw batch decode failure with ${partsPerRecord} parts closes decoded records and preserves the remainder`, async t => {
    const factory = new backend.ZLinkNodeBackendAdapterFactory();
    const context = factory.createChannelAdapter().createContext();
    const queue = applicationQueue(3n);
    const node = factory.createMeshAdapter().createMeshNode(context, {
      meshName: 'batch-decode-failure', routingId: `batch-decode-${process.pid}`,
      applicationJobQueue: queue
    });
    const readyBatch = node.createReadyBatch(1);
    const receiveBatch = node.createReceiveBatch(64, 256);
    const failure = new Error('message materialization failed');
    let decoded = 0;
    let closes = 0;
    const fromOwned = ZLinkBufferMessage.fromOwned;
    try {
      node.setBind(`inproc://batch-decode-${process.pid}`);
      node.addChannelName('batch');
      node.start();
      for (let sequence = 0; sequence < 3; sequence++) {
        await node.sendToChannel('batch', Array.from(
          { length: partsPerRecord }, (_, part) => Buffer.from(`${sequence}-${part}`)
        ));
      }
      assert.equal(queue.snapshot().permitsInUse, 3n);
      t.mock.method(ZLinkBufferMessage, 'fromOwned', function (data) {
        if (++decoded === 2 * partsPerRecord) throw failure;
        const message = fromOwned.call(this, data);
        const close = message.close.bind(message);
        message.close = () => { closes++; close(); };
        return message;
      });
      assert.equal(node.drainReady(ReadyDomain.Application, readyBatch).hasResidue, false);
      const first = readyBatch.takeClaim(0);
      try {
        assert.throws(() => first.recvBatch(receiveBatch), error => error === failure);
      } finally {
        first.release();
      }
      assert.equal(closes, 2 * partsPerRecord - 1, 'both complete and partial records stay owned until returned');
      assert.equal(queue.snapshot().permitsInUse, 1n, 'only the unconsumed record retains a permit');
      readyBatch.reset();
      receiveBatch.reset();
      assert.equal(node.drainReady(ReadyDomain.Application, readyBatch).records.length, 1);
      const remainder = readyBatch.takeClaim(0);
      try {
        const received = remainder.recvBatch(receiveBatch);
        assert.deepEqual(received.records.map(record => record.parts[0].data().toString()), ['2-0']);
        for (const record of received.records) {
          for (const part of record.parts) part.close();
          record.releaseRetainedIngress();
        }
      } finally {
        remainder.release();
      }
      assert.equal(closes, 3 * partsPerRecord - 1);
      assert.equal(queue.snapshot().permitsInUse, 0n);
    } finally {
      receiveBatch.close();
      readyBatch.close();
      node.close();
      await context.dispose();
    }
  });
}

test('a reused receive batch applies the current admission limit without changing its allocation', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const queue = applicationQueue(3n);
  const node = factory.createMeshAdapter().createMeshNode(context, {
    meshName: 'batch-limit', routingId: `batch-limit-${process.pid}`, applicationJobQueue: queue
  });
  const readyBatch = node.createReadyBatch(1);
  const receiveBatch = node.createReceiveBatch(64, 256);
  const received = [];
  try {
    node.setBind(`inproc://batch-limit-${process.pid}`);
    node.addChannelName('batch');
    node.start();
    for (let sequence = 0; sequence < 3; sequence++) {
      await node.sendToChannel('batch', Buffer.from(String(sequence)));
    }
    for (const limit of [1, 64]) {
      readyBatch.reset();
      receiveBatch.reset(limit);
      assert.equal(node.drainReady(ReadyDomain.Application, readyBatch).records.length, 1);
      const claim = readyBatch.takeClaim(0);
      try {
        const result = claim.recvBatch(receiveBatch);
        try {
          assert.equal(result.records.length, limit === 1 ? 1 : 2);
          received.push(...result.records.map(record => record.parts[0].data().toString()));
        } finally {
          for (const record of result.records) {
            for (const part of record.parts) part.close();
            record.releaseRetainedIngress();
          }
        }
      } finally {
        claim.release();
      }
    }
    assert.deepEqual(received, ['0', '1', '2']);
    assert.equal(queue.snapshot().permitsInUse, 0n);
    for (const invalid of [0, -1, 65, 1.5, NaN]) {
      assert.throws(() => receiveBatch.reset(invalid), RangeError);
    }
  } finally {
    receiveBatch.close();
    readyBatch.close();
    node.close();
    await context.dispose();
  }
});
