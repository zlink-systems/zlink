const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkMeshCompletionTable
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');

// Growing the completion table to N pending operations re-indexes only when
// the bucket array doubles, and every pending identity stays reachable.
test('completion table growth rebuilds buckets only when they double', async (t) => {
  let bucketArrays = 0;
  const NativeInt32Array = globalThis.Int32Array;
  t.mock.method(
    globalThis,
    'Int32Array',
    class extends NativeInt32Array {
      constructor(...args) {
        super(...args);
        bucketArrays++;
      }
    }
  );
  const table = new ZLinkMeshCompletionTable();
  const pending = [];
  for (let low = 1n; low <= 1024n; low++) {
    pending.push(table.submit(() => ({ high: 1n, low })));
  }
  t.mock.restoreAll();
  // The empty initial array, then 128 doubling to 2048 entries for 1024 pending.
  assert.ok(bucketArrays <= 6, `bucket arrays built while growing to 1024: ${bucketArrays}`);
  for (let low = 1n; low <= 1024n; low++) {
    table.complete({
      operationId: { high: 1n, low },
      terminalResult: 0,
      failureErrno: 0,
      operationKind: 1,
      kindData: null,
      parts: []
    });
  }
  const completions = await Promise.all(pending);
  assert.equal(completions.length, 1024);
  assert.equal(table.pendingCount, 0);
  table.dispose();
});
