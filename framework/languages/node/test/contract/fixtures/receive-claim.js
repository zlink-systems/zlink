const assert = require('node:assert/strict');
const { ZLinkMeshDispatchPump } = require('../../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const { ReadyDomain } = require('../../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

async function exerciseClaims(instrument) {
  let ready;
  let queued = false;
  let onReceiveStack = false;
  const counts = { dispatch: 0, terminal: 0, release: 0, close: 0 };
  const owner = { terminalCompletion: true };
  const record = { parts: [], onTerminalCompletion() { counts.terminal += 1; } };
  const claim = {
    recvBatch() { return { ok: true, records: [record] }; },
    release() { counts.release += 1; }
  };
  const batch = () => ({ reset() {}, close() { counts.close += 1; } });
  const node = {
    setReadyHandler(handler) { ready = handler; },
    createReadyBatch() { return { ...batch(), takeClaim() { return claim; } }; },
    createReceiveBatch: batch,
    drainReady() {
      if (!queued) return { ok: false, records: [] };
      queued = false;
      return { ok: true, hasResidue: false, records: [owner] };
    }
  };
  const pump = new ZLinkMeshDispatchPump(node, {
    applicationJobQueue: {},
    monotonicNowMs: () => 0,
    dispatch() {
      assert.equal(onReceiveStack, false);
      counts.dispatch += 1;
      return Promise.resolve();
    },
    reportError(error) { throw error; }
  });
  pump.start();
  await new Promise(setImmediate);
  instrument(pump);
  try {
    for (let index = 0; index < 1000; index += 1) {
      queued = true;
      onReceiveStack = true;
      try { ready(ReadyDomain.Application); } finally { onReceiveStack = false; }
      await new Promise(setImmediate);
      assert.equal(counts.release, index + 1);
    }
  } finally {
    await pump.dispose();
  }
  assert.deepEqual(counts, { dispatch: 1000, terminal: 1000, release: 1000, close: 2 });
  return counts;
}

module.exports = { exerciseClaims };
