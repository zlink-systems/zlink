const assert = require('node:assert/strict');
const test = require('node:test');
const { allocationTrace } = require('./fixtures/receive-allocation-probe');
let result;
test.before(() => { result = allocationTrace('receive'); });
for (const cost of ['iterator', 'capture']) {
  test(`synchronous raw receive avoids ${cost} allocations and preserves byte and close accounting`, (t) => {
    t.diagnostic(JSON.stringify(result));
    assert.equal(result.counts.records, 1000);
    assert.equal(result.counts.bytes, 1056000);
    assert.equal(result.counts.closes, 1000);
    assert.equal(result.counts.releases, 1000);
    if (cost === 'iterator') assert.equal(result.counts.iterators, 0);
    else assert.equal(result.allocations.receiveOne, 0);
  });
}
