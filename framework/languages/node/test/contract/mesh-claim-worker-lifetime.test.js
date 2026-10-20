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

test('1000 synchronous pre-admitted claims stay within the pump allocation budget', (t) => {
  const { allocationTrace } = require('./fixtures/receive-allocation-probe');
  const result = allocationTrace('claim');
  t.diagnostic(JSON.stringify(result));
  assert.deepEqual(result.counts, {
    dispatch: 1000, terminal: 1000, release: 1000, permitRelease: 1000, close: 2
  });
  // Count allocations attributed directly to this module, excluding Node's
  // Promise/timer internals. One extra closure per claim exceeds this budget.
  const maximumPumpAllocations = 6000;
  assert.ok(result.moduleAllocations.count <= maximumPumpAllocations,
    `pump allocated ${result.moduleAllocations.count} objects; maximum ${maximumPumpAllocations}`);
});
