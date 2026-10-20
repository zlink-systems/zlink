const assert = require('node:assert/strict');
const { createHook } = require('node:async_hooks');
const test = require('node:test');
const { fixture } = require('./fixtures/raw-mesh-close');

test('synchronous raw receive rotations do not create discarded runner Promises', async (t) => {
  const f = fixture();
  await f.backend.pumpTask;
  let rotations = 0;
  let promises = 0;
  f.runtime.pumpBatch = () => { rotations += 1; return false; };
  // The init hook observes actual V8 Promise creation, including async return values.
  const hook = createHook({ init(_id, type) {
    if (type === 'PROMISE' && /at ZLinkNodeRawMeshBackend.runPump/.test(new Error().stack)) {
      promises += 1;
    }
  } });
  hook.enable();
  try {
    for (let index = 0; index < 1000; index += 1) f.readable(true, false);
  } finally {
    hook.disable();
    await f.backend.close();
  }
  t.diagnostic(JSON.stringify({ rotations, promises }));
  assert.equal(rotations, 1000);
  assert.equal(promises, 0);
});
