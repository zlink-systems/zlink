'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const { assertClaimState } = require('./fixtures/claim-state-snapshot');
const { ZLinkMeshDispatchPump } = require('../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

// Exercise the registered readiness callback, including worker scheduling and shutdown.
function harness(domain, options, owner, claims) {
  let ready;
  let next = 0;
  const receiveBatches = [];
  const pump = new ZLinkMeshDispatchPump({
    setReadyHandler(handler) { ready = handler; },
    createReadyBatch() {
      let claim;
      return { reset() {}, close() {}, takeClaim() { return claim; },
        setClaim(value) { claim = value; } };
    },
    createReceiveBatch() {
      const batch = { reset() {}, close() { this.closed = true; } };
      receiveBatches.push(batch);
      return batch;
    },
    drainReady(requested, batch) {
      if (requested !== domain || next === claims.length) return { ok: false, records: [] };
      batch.setClaim(claims[next++]);
      return { ok: true, hasResidue: false, records: [owner] };
    }
  }, { monotonicNowMs: () => 0, reportError(error) { throw error; }, ...options });
  pump.start();
  return { pump, receiveBatches, ready() { ready(domain); } };
}

for (const preparation of ['true', 'false', 'throw', 'async-true', 'async-false', 'reject', 'abort']) {
  test(`claim preparation preserves record completion and ownership: ${preparation}`, async () => {
    const events = [];
    const errors = [];
    const failure = new Error('source preparation failed');
    let resume;
    let signal;
    const records = [0, 1].map((index) => ({
      parts: [{ data() { return Buffer.alloc(0); }, close() { events.push(`part-${index}`); } }],
      applicationJobPermit: {
        releaseAfterInternalProcessing() { events.push(`permit-${index}`); }
      },
      onTerminalCompletion() { events.push(`terminal-${index}`); },
      releaseRetainedIngress() { events.push(`retained-${index}`); }
    }));
    records[0].prepareDispatch = (stop) => {
      signal = stop;
      events.push('prepare');
      if (preparation === 'throw') throw failure;
      if (preparation === 'reject') return Promise.reject(failure);
      if (preparation === 'abort') return new Promise((resolve) => { resume = resolve; });
      const accepted = preparation.endsWith('true');
      return preparation.startsWith('async-') ? Promise.resolve(accepted) : accepted;
    };
    const h = harness(ReadyDomain.Application, {
      applicationJobQueue: {},
      dispatch(_owner, record) {
        events.push(`dispatch-${records.indexOf(record)}`);
        return Promise.resolve();
      },
      reportError(error) { errors.push(error); }
    }, { ordinaryIngressPreAdmitted: true }, [{
      recvBatch() { return { ok: true, records }; },
      release() { events.push('claim'); }
    }]);
    try {
      h.ready();
      await new Promise(setImmediate);
      if (preparation === 'abort') {
        assert.deepEqual(events, ['prepare']);
        const closing = h.pump.dispose();
        assert.equal(signal.aborted, true);
        resume(true);
        await closing;
      }
      const accepted = preparation === 'true' || preparation === 'async-true';
      assert.equal(events.includes('dispatch-0'), accepted);
      assert.equal(events.includes('terminal-0'), accepted);
      assert.equal(events.includes('dispatch-1'), preparation !== 'abort');
      assert.equal(events.includes('terminal-1'), preparation !== 'abort');
      assert.deepEqual(errors, ['throw', 'reject'].includes(preparation) ? [failure] : []);
      for (const index of [0, 1]) {
        assert.equal(events.filter((event) => event === `part-${index}`).length, 1);
        assert.equal(events.filter((event) => event === `retained-${index}`).length, 1);
        if (preparation !== 'abort' || index === 0)
          assert.equal(events.filter((event) => event === `permit-${index}`).length, 1);
      }
      assert.equal(events.filter((event) => event === 'claim').length, 1);
    } finally {
      resume?.(true);
      await h.pump.dispose();
    }
  });
}

for (const domain of [ReadyDomain.Application, ReadyDomain.Infrastructure]) {
  test(`single synchronous claim creates no Promise: domain ${domain}`, async () => {
    const events = [];
    let promises = 0;
    const hook = createHook({ init(_id, type) {
      if (type === 'PROMISE' && new Error().stack.includes('at ZLinkMeshDispatchPump.dispatchClaim ')) promises += 1;
    } });
    const record = {
      parts: [{ close() { events.push('part'); } }],
      onTerminalCompletion() { events.push('terminal'); },
      releaseRetainedIngress() { events.push('retained'); }
    };
    const h = harness(domain, {
      applicationJobQueue: {},
      dispatch() { events.push('dispatch'); }
    }, { terminalCompletion: true }, [{
      recvBatch() {
        queueMicrotask(() => {
          assert.deepEqual(events, ['dispatch', 'terminal', 'part', 'retained', 'claim']);
        });
        return { ok: true, records: [record] };
      },
      release() { events.push('claim'); }
    }]);
    hook.enable();
    try {
      h.ready();
      await new Promise(setImmediate);
      assert.equal(promises, 0);
      assert.deepEqual(events, ['dispatch', 'terminal', 'part', 'retained', 'claim']);
    } finally { hook.disable(); await h.pump.dispose(); }
  });
}

test('single claim retains ownership through pending dispatch and terminal completion', async () => {
  const events = [];
  let dispatchDone;
  let terminalDone;
  const dispatch = new Promise((resolve) => { dispatchDone = resolve; });
  const terminal = new Promise((resolve) => { terminalDone = resolve; });
  const h = harness(ReadyDomain.Infrastructure, {
    applicationJobQueue: {},
    dispatch() { events.push('dispatch'); return dispatch; }
  }, {}, [{
    recvBatch() { return { ok: true, records: [{
      parts: [{ close() { events.push('part'); } }],
      onTerminalCompletion() { events.push('terminal'); return terminal; }
    }] }; },
    release() { events.push('claim'); }
  }]);
  try {
    h.ready();
    await new Promise(setImmediate);
    assert.deepEqual(events, ['dispatch']);
    dispatchDone();
    await new Promise(setImmediate);
    assert.deepEqual(events, ['dispatch', 'terminal']);
    terminalDone();
    await new Promise(setImmediate);
    assert.deepEqual(events, ['dispatch', 'terminal', 'part', 'claim']);
  } finally { dispatchDone(); terminalDone(); await h.pump.dispose(); }
});

test('empty ordinary receive returns its unused permit before releasing the claim', async () => {
  const events = [];
  const h = harness(ReadyDomain.Application, {
    applicationJobQueue: { acquire() {
      events.push('permit');
      return Promise.resolve({ releaseAfterInternalProcessing() { events.push('permit-release'); } });
    } },
    dispatch() { assert.fail('empty receive must not dispatch'); }
  }, {}, [{
    recvBatch() { events.push('receive'); return { ok: false, records: [] }; },
    release() { events.push('claim'); }
  }]);
  try {
    h.ready();
    await new Promise(setImmediate);
    assert.deepEqual(events, ['permit', 'receive', 'permit-release', 'claim']);
  } finally { await h.pump.dispose(); }
});

test('worker claim state releases suspended ownership before reuse and empty receive', async () => {
  const events = [];
  let resume;
  let firstBatch;
  const suspended = new Promise((resolve) => { resume = resolve; });
  const record = {
    parts: [{ close() { events.push('part'); } }],
    applicationJobPermit: { releaseAfterInternalProcessing() { events.push('permit'); } },
    onTerminalCompletion() { events.push('terminal'); }
  };
  const h = harness(ReadyDomain.Application, {
    applicationJobQueue: {}, dispatch() { return suspended; }
  }, { ordinaryIngressPreAdmitted: true }, [{
    recvBatch(batch) { firstBatch = batch; return { ok: true, records: [record] }; },
    release() { events.push('first'); }
  }, {
    recvBatch(batch) {
      assert.strictEqual(batch, firstBatch);
      assert.equal(batch.closed, undefined, 'worker retains its receive batch between claims');
      assert.deepEqual(events, ['terminal', 'permit', 'part', 'first']);
      return { ok: false, records: [] };
    },
    release() { events.push('second'); }
  }]);
  try {
    h.ready();
    await new Promise(setImmediate);
    assert.deepEqual(events, []);
    assert.equal(h.receiveBatches.length, 1);
    await assertClaimState(firstBatch, record);
    resume();
    await new Promise(setImmediate);
    assert.deepEqual(events, ['terminal', 'permit', 'part', 'first', 'second']);
    await assertClaimState(firstBatch);
    assert.equal(firstBatch.closed, undefined, 'worker retains its batch while idle');
  } finally { resume(); await h.pump.dispose(); }
});
