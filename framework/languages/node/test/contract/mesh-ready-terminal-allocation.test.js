const assert = require('node:assert/strict');
const test = require('node:test');
const { allocationTrace } = require('./fixtures/receive-allocation-probe');
let result;
test.before(() => { result = allocationTrace('pump'); });
for (const name of ['markReady', 'finishRecord']) {
  test(`synchronous ${name} does not allocate pending continuation captures`, (t) => {
    t.diagnostic(JSON.stringify(result));
    assert.deepEqual(result.counts, { calls: 1000, terminals: 1000 });
    assert.equal(result.allocations[name], 0);
  });
}
