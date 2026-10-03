'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { getEventListeners } = require('node:events');
const { awaitWithAbort, ZLinkAbortError } = require('../../packages/framework/dist/runtime/abort');

test('already aborted wait observes late rejection and invokes cancellation once', async () => {
  const controller = new AbortController();
  controller.abort();
  let rejectOperation;
  const operation = new Promise((_resolve, reject) => { rejectOperation = reject; });
  let cancellations = 0;
  await assert.rejects(awaitWithAbort(operation, controller.signal, () => { cancellations++; }), ZLinkAbortError);
  rejectOperation(new Error('late terminal'));
  await new Promise(setImmediate);
  assert.equal(cancellations, 1);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
});

test('a terminal operation releases its abort listener before later cancellation', async () => {
  const controller = new AbortController();
  let cancellations = 0;
  assert.equal(await awaitWithAbort(Promise.resolve('reply'), controller.signal, () => { cancellations++; }), 'reply');
  controller.abort();
  assert.equal(cancellations, 0);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
});
