'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const { fixture } = require('./fixtures/raw-mesh-close');

test('1000 completed readiness rotations add no pump startup Promise continuation', async () => {
  const f = fixture();
  await f.backend.pumpTask;
  let rotations = 0;
  let continuations = 0;
  const startup = new Set();
  const hook = createHook({
    init(id, type) {
      if (type === 'PROMISE' && /at ZLinkNodeRawMeshBackend.runPump/.test(new Error().stack)) {
        startup.add(id);
      }
    },
    before(id) { if (startup.has(id)) continuations++; }
  });
  f.runtime.pumpBatch = () => { rotations++; return false; };
  try {
    hook.enable();
    for (let index = 0; index < 1000; index++) {
      f.readable(true, false);
      await f.backend.pumpTask;
    }
    hook.disable();
    assert.equal(rotations, 1000);
    assert.equal(continuations, 0, 'completed receive work needs no startup handoff');
  } finally {
    hook.disable();
    await f.backend.close();
  }
});

for (const stage of ['reporting', 'maintenance']) {
  test(`pump join retains ${stage} failures without a detached rejection`, async () => {
    const f = fixture();
    await f.backend.pumpTask;
    let owned;
    const error = new Error(`${stage} failed`);
    const originalMaintenance = f.backend.scheduleMaintenance;
    f.runtime.pumpBatch = () => {
      owned = f.backend.pumpTask;
      if (stage === 'reporting') throw new Error('receive failed');
      return false;
    };
    if (stage === 'reporting') f.runtime.reportInfrastructureFailure = () => { throw error; };
    else f.backend.scheduleMaintenance = () => { throw error; };
    try {
      f.readable(true, false);
      assert.ok(owned instanceof Promise);
      await assert.rejects(owned, error);
      assert.equal(f.backend.pumpTask, undefined);
    } finally {
      f.backend.scheduleMaintenance = originalMaintenance;
      await f.backend.close();
    }
  });
}
