'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { fixture } = require('./fixtures/raw-mesh-close');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

test('terminal records reuse stateless rejection callbacks across operations', async t => {
  const f = fixture();
  const records = [];
  let done;
  const completed = new Promise(resolve => { done = resolve; });
  const count = 64;
  f.backend.setCompletionHandler((_terminal, materialize) => {
    records.push(materialize());
    if (records.length === count) done();
  });
  try {
    for (let index = 0; index < count; index++)
      f.backend.requestToNode('missing', [Buffer.from('request')]);
    await completed;
    for (const record of records) {
      assert.equal(record.reply([]), SubmitResult.InvalidState);
      assert.equal(record.replyActorJoin([]), SubmitResult.NotSupported);
    }
    const replyFunctions = new Set(records.map(record => record.reply)).size;
    const joinFunctions = new Set(records.map(record => record.replyActorJoin)).size;
    t.diagnostic(`${count} completions: reply functions=${replyFunctions}, join functions=${joinFunctions}`);
    assert.equal(replyFunctions, 1);
    assert.equal(joinFunctions, 1);
  } finally { await f.backend.close(); }
});
