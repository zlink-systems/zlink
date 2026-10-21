const assert = require('node:assert/strict');
const test = require('node:test');
const { allocationTrace } = require('./fixtures/receive-allocation-probe');
let result;
test.before(() => { result = allocationTrace('dispatch'); });
for (const name of ['processReceived', 'processApplicationReceived']) {
  test(`synchronous ${name} does not allocate pending dispatch captures`, (t) => {
    t.diagnostic(JSON.stringify(result));
    assert.equal(result.counts.calls, 1000);
    assert.equal(result.allocations[name], 0);
  });
}
