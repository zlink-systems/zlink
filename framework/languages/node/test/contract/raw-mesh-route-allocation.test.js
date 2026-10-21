const assert = require('node:assert/strict');
const test = require('node:test');
const { allocationTrace } = require('./fixtures/receive-allocation-probe');

test('ordinary raw pump does not allocate the route-only continuation capture', (t) => {
  const result = allocationTrace('route');
  t.diagnostic(JSON.stringify(result));
  assert.equal(result.counts.calls, 1000);
  assert.equal(result.allocations.pumpBatch, 0);
});
