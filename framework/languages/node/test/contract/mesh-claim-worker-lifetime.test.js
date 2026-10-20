const assert = require('node:assert/strict');
const test = require('node:test');
const { exerciseClaims } = require('./fixtures/receive-claim');

test('application worker reuses its claim frame across dispatch and terminal cleanup', async (t) => {
  let claimPromises = 0;
  const counts = await exerciseClaims((pump) => {
    const drain = pump.drainClaim;
    pump.drainClaim = function (...args) {
      const result = drain.apply(this, args);
      if (args[2] !== undefined && result instanceof Promise) claimPromises += 1;
      return result;
    };
  });
  t.diagnostic(JSON.stringify({ ...counts, claimPromises }));
  assert.equal(claimPromises, 0, 'a shared application worker must not create a per-claim async result');
});
