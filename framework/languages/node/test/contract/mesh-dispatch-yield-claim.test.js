'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkMeshDispatchPump } = require('../../packages/framework/dist/runtime/backend');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { ServiceMailbox } = require('../../packages/framework/dist/runtime/foundation/service-mailbox');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } =
  require('../../packages/framework/dist/runtime/host/application-job-queue');

test('empty ready claims yield at the count boundary after releasing the claim', async (t) => {
  let ready;
  let received = 0;
  let claimsHeld = 0;
  const timers = [];
  const node = {
    setReadyHandler(handler) { ready = handler; },
    createReadyBatch() {
      return {
        reset() {}, close() {},
        takeClaim() {
          claimsHeld += 1;
          return {
            recvBatch() { received += 1; return { ok: false, records: [] }; },
            release() { claimsHeld -= 1; }
          };
        }
      };
    },
    createReceiveBatch() { return { reset() {}, close() {} }; },
    drainReady(domain) {
      if (domain !== ReadyDomain.Application) return { ok: false, hasResidue: false, records: [] };
      return { ok: true, hasResidue: false, records: [{ ordinaryIngressPreAdmitted: true }] };
    }
  };
  t.mock.method(global, 'setTimeout', (callback, delay) => {
    assert.equal(delay, 0);
    timers.push({ callback, received, claimsHeld });
    return { unref() {} };
  });
  const pump = new ZLinkMeshDispatchPump(node, {
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({})),
    monotonicNowMs: () => 0,
    dispatch() { assert.fail('empty claims must not dispatch'); }
  });
  try {
    pump.start();
    ready(ReadyDomain.Application);
    await new Promise(( resolve ) => setImmediate(resolve));
    assert.deepEqual(timers.map(({ received, claimsHeld }) => ({ received, claimsHeld })),
      [{ received: 16, claimsHeld: 0 }]);
  } finally {
    const stopping = pump.dispose();
    for (const timer of timers) timer.callback();
    await stopping;
  }
});

for (const trigger of ['elapsed', 'records']) {
  test(`dispatch ${trigger} yield releases the claim and admits the next handler before its timer`, async (t) => {
    const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({
      maxQueuedApplicationJobs: 64n
    }));
    const permits = [];
    const count = trigger === 'records' ? 16 : 1;
    for (let i = 0; i <= count; i++) {
      const permit = await queue.acquire(undefined, 'remote');
      permit.markApplicationQueued();
      permits.push(permit);
    }
    let ready;
    let now = 0;
    const claims = new Set();
    const timers = [];
    const errors = [];
    const dispatched = [];
    const mailbox = new ServiceMailbox(() => ready(ReadyDomain.Application));
    const enqueue = (sequence) => assert.equal(mailbox.tryEnqueue({
      owner: 'spot:one', domain: 'application', parts: [Buffer.from([sequence])],
      stateful: { sequence, parts: [], applicationJobPermit: permits[sequence] }
    }), true);
    const node = {
      setReadyHandler(handler) { ready = handler; },
      createReadyBatch() {
        return {
          reset() {}, close() {},
          takeClaim() {
            const claim = claims.values().next().value;
            let received = false;
            return {
              recvBatch() {
                if (received) return { ok: false, records: [] };
                received = true;
                return { ok: true, records: claim.records.map(( record ) => record.stateful) };
              },
              release() { mailbox.release(claim); claims.delete(claim); }
            };
          }
        };
      },
      createReceiveBatch() { return { reset() {}, close() {} }; },
      drainReady(domain) {
        assert.equal(domain, ReadyDomain.Application);
        const claim = mailbox.tryClaim('application', 64, 1024);
        if (!claim) return { ok: false, hasResidue: false, records: [] };
        claims.add(claim);
        return { ok: true, hasResidue: false, records: [{ ordinaryIngressPreAdmitted: true }] };
      }
    };
    t.mock.method(global, 'setTimeout', (callback, delay) => {
      assert.equal(delay, 0);
      timers.push({ callback, claimsHeld: claims.size });
      // The completed caller submits its next request while the timer stays pending.
      if (timers.length === 1) enqueue(count);
      return { unref() {} };
    });
    const pump = new ZLinkMeshDispatchPump(node, {
      applicationJobQueue: queue,
      monotonicNowMs: () => now,
      dispatch(_owner, record) {
        dispatched.push(record.sequence);
        if (trigger === 'elapsed') now = 2;
      },
      reportError(error) { errors.push(error); }
    });
    try {
      pump.start();
      for (let i = 0; i < count; i++) enqueue(i);
      await new Promise(( resolve ) => setImmediate(resolve));
      assert.ok(timers.length > 0, 'the unchanged fairness condition must cause a timer yield');
      assert.deepEqual({ claimsHeld: timers[0].claimsHeld, dispatched }, {
        claimsHeld: 0,
        dispatched: Array.from({ length: count + 1 }, (_, i) => i)
      }, 'release the claim and enter the next handler before completing the yield timer');
      assert.ok(timers.every(( timer ) => timer.claimsHeld === 0));
      assert.deepEqual(errors, []);
      assert.equal(queue.snapshot().permitsInUse, 0n);
    } finally {
      const stopping = pump.dispose();
      for (const timer of timers) timer.callback();
      await stopping;
      mailbox.close();
      for (const permit of permits) permit.releaseAfterInternalProcessing();
    }
    assert.equal(claims.size, 0);
    assert.equal(queue.snapshot().permitsInUse, 0n);
  });
}
