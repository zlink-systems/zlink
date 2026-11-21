'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const {
  ZLinkMeshDispatchPump
} = require('../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const {
  ReadyDomain
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

for (const count of [2, 32]) {
  test(`each worker keeps its own continuous time budget across ${count} same-owner takes`, async (t) => {
    const workerIds = new WeakMap();
    let currentWorker;
    let ready,
      clock = 0,
      nextWorker = 0,
      remaining = count;
    const starts = new Map();
    const timers = [];
    const errors = [];
    let claim;
    const node = {
      setReadyHandler(handler) {
        ready = handler;
      },
      createReadyBatch() {
        const batch = {
          reset() {},
          close() {},
          takeClaim() {
            return claim;
          }
        };
        workerIds.set(batch, ++nextWorker);
        return batch;
      },
      createReceiveBatch() {
        return { reset() {}, close() {} };
      },
      drainReady(_domain, batch) {
        currentWorker = workerIds.get(batch);
        if (remaining === 0) return { ok: false, records: [] };
        const id = remaining--;
        claim = {
          recvBatch() {
            return {
              ok: true,
              records: [
                { id, parts: [], applicationJobPermit: { releaseAfterInternalProcessing() {} } }
              ]
            };
          },
          release() {}
        };
        return {
          ok: true,
          hasResidue: remaining > 0,
          records: [{ ordinaryIngressPreAdmitted: true }]
        };
      }
    };
    t.mock.method(global, 'setTimeout', (callback) => {
      const worker = currentWorker;
      timers.push({ callback, worker, count: starts.get(worker), clock });
      return { unref() {} };
    });
    const pump = new ZLinkMeshDispatchPump(node, {
      applicationJobQueue: {},
      monotonicNowMs: () => clock,
      dispatch() {
        const worker = currentWorker;
        starts.set(worker, (starts.get(worker) ?? 0) + 1);
        clock += 1;
      },
      reportError(error) {
        errors.push(error);
      }
    });
    try {
      pump.start();
      ready(ReadyDomain.Application);
      // Timers stay mocked and never fire here, so every record taken below
      // comes from workers continuing without a yield. How many setImmediate
      // turns that takes depends on the platform's event-loop ordering
      // (macOS needed more than one), so wait for the pump to go quiet.
      for (let turns = 0; turns < 20 && remaining > 0; turns += 1) await turn();
      if (count === 2)
        assert.equal(timers.length, 0, 'another worker consumes no part of this worker budget');
      else {
        assert.equal(remaining, 0, 'other workers continue while earlier workers yield');
        assert.ok(timers.length >= 2);
        assert.ok(
          timers.every((timer) => timer.count === 10),
          'each worker starts ten records before its own yield'
        );
        assert.ok(new Set(timers.map((timer) => timer.worker)).size >= 2);
      }
      assert.deepEqual(errors, []);
    } finally {
      const stopping = pump.dispose();
      for (const timer of timers) timer.callback();
      await stopping;
    }
  });
}
