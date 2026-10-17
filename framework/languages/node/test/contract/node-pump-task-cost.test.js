'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { fixture } = require('./fixtures/raw-mesh-close');

test('pump progress reuses its owned async task without Promise wrapper chains', async t => {
  const f = fixture();
  let active = false, thenCalls = 0;
  const originalThen = Promise.prototype.then;
  t.mock.method(Promise.prototype, 'then', function (...args) {
    if (active) thenCalls++;
    return originalThen.apply(this, args);
  });
  f.runtime.pumpBatch = async () => false;
  try {
    f.readable(true, false); await f.backend.pumpTask;
    active = true;
    f.readable(true, false);
    await f.backend.pumpTask;
    active = false;
    assert.equal(thenCalls, 0);
  } finally { active = false; await f.backend.close(); }
});

test('readiness raised inside a round retains the same pump task and close join', async () => {
  const f = fixture();
  let rounds = 0, owned;
  f.runtime.pumpBatch = () => {
    rounds++;
    assert.strictEqual(f.backend.pumpTask, owned);
    if (rounds === 1) f.readable(true, false);
    return false;
  };
  try {
    f.readable(true, false);
    owned = f.backend.pumpTask;
    assert.ok(owned instanceof Promise);
    assert.equal(rounds, 0, 'callbacks begin after the task is installed');
    await owned;
    assert.equal(rounds, 2);
    assert.equal(f.backend.pumpTask, undefined);
  } finally { await f.backend.close(); }
});
