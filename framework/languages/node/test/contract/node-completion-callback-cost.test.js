const { ZLinkMeshCompletionTable } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { threadId } = require('node:worker_threads');
const { fixture } = require('./fixtures/raw-mesh-close');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

test('messaging hot path completion transition count is zero on the event loop', async () => {
  const table = new ZLinkMeshCompletionTable();
  const operationId = { high: 1n, low: 1n };
  const completion = table.submit(() => operationId);
  const notificationThreadId = threadId;
  const transitionCount = completion.then(() =>
    notificationThreadId === threadId ? 0 : 1
  );

  table.complete({
    operationId,
    terminalResult: 0,
    failureErrno: 0,
    operationKind: 0,
    kindData: null,
    parts: []
  });

  assert.equal(await transitionCount, 0);
});

test('terminal records reuse stateless rejection callbacks across operations', async t => {
  const f = fixture();
  const records = [];
  let done;
  const completed = new Promise(resolve => { done = resolve; });
  const count = 64;
  const table = new ZLinkMeshCompletionTable();
  f.backend.setCompletionHandler((terminal, materialize) => table.complete(terminal, materialize));
  t.mock.method(table, 'complete', (_terminal, materialize) => {
    records.push(materialize());
    if (records.length === count) done();
  });
  try {
    for (let index = 0; index < count; index++)
      f.backend.requestToNode('missing', [Buffer.from('request')], {});
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
