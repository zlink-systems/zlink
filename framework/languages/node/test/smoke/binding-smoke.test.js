const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');

test('binding public API exposes version', () => {
  const actual = zlink.version();
  assert.equal(Array.isArray(actual), true);
  assert.equal(actual.length, 3);
  assert.equal(actual.every((part) => Number.isInteger(part)), true);
});

test('framework adapter reads binding version through public API', () => {
  const backend = require('../../packages/framework/dist/runtime/backend');
  const info = backend.getNodeBindingInfo();
  assert.deepEqual(info.version, zlink.version());
});
