const assert = require('node:assert/strict');
const test = require('node:test');

const {
  createAbortError,
  ZLinkAbortError,
  throwIfAborted
} = require('../../packages/framework/dist/runtime/abort');

test('framework abort policy uses the stable typed cancellation error contract', () => {
  const error = createAbortError();

  assert.equal(error.constructor, ZLinkAbortError);
  assert.equal(error instanceof ZLinkAbortError, true);
  assert.equal(error.message, 'The operation was aborted.');
  assert.doesNotThrow(() => throwIfAborted(undefined));
  assert.doesNotThrow(() => throwIfAborted(new AbortController().signal));

  const controller = new AbortController();
  controller.abort();
  assert.throws(
    () => throwIfAborted(controller.signal),
    (cause) => cause instanceof ZLinkAbortError && cause.message === 'The operation was aborted.'
  );
});
