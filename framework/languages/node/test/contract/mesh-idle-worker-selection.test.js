const assert = require('node:assert/strict');
const test = require('node:test');
const { exerciseClaims } = require('./fixtures/receive-claim');

test('waking an idle application worker does not create a selection iterator', async (t) => {
  let iterators = 0;
  const counts = await exerciseClaims(() => (notify) => {
    // Observe collection iteration only during the public readiness callback.
    // Cover both collection types so the previous Set-based worker fails too.
    const arrayValues = Array.prototype.values;
    const setValues = Set.prototype.values;
    Array.prototype.values = function () {
      iterators += 1;
      return arrayValues.call(this);
    };
    Set.prototype.values = function () {
      iterators += 1;
      return setValues.call(this);
    };
    try {
      notify();
    } finally {
      Array.prototype.values = arrayValues;
      Set.prototype.values = setValues;
    }
  });
  t.diagnostic(JSON.stringify({ ...counts, iterators }));
  assert.equal(iterators, 0);
});
