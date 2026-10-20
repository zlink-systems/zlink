const assert = require('node:assert/strict');
const test = require('node:test');
const { exerciseClaims } = require('./fixtures/receive-claim');

test('waking an idle application worker does not create a selection iterator', async (t) => {
  let iterators = 0;
  const counts = await exerciseClaims((pump) => {
    const idle = pump.idleApplicationWorkers;
    const values = idle.values;
    idle.values = function () { iterators += 1; return values.call(this); };
  });
  t.diagnostic(JSON.stringify({ ...counts, iterators }));
  assert.equal(iterators, 0);
});
