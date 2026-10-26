'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkEncodedPayload
} = require('../../packages/framework/dist/contracts/Common/ZLinkEncodedPayload');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  borrowEncodedPayload
} = require('../../packages/framework/dist/contracts/Common/encoded-payload-storage');

function storageCalls(spies, owner) {
  return spies.reduce(
    (count, spy) => count + spy.mock.calls.filter((call) => call.arguments[0] === owner).length,
    0
  );
}

test('encoded payload ownership uses no side table beyond the direct Buffer owner', (t) => {
  const bytes = Buffer.from('owned payload');
  const spies = ['set', 'get'].map((name) => t.mock.method(WeakMap.prototype, name));
  const direct = ZLinkBufferMessage.fromOwned(bytes);
  assert.strictEqual(direct.data(), bytes);
  const baseline = storageCalls(spies, direct);
  const payload = ZLinkEncodedPayload.fromOwned(bytes);
  assert.strictEqual(payload.view(), bytes);
  assert.strictEqual(borrowEncodedPayload(payload), bytes);
  assert.equal(payload.size(), direct.size());
  assert.equal(payload.isEmpty(), direct.isEmpty());
  assert.equal(payload.getString(), direct.getString());
  assert.deepEqual(payload.data(), new Uint8Array(bytes));
  assert.deepEqual(payload.toBytes(), new Uint8Array(bytes));
  assert.deepEqual(Object.getOwnPropertyNames(payload), []);
  assert.equal(storageCalls(spies, payload), baseline);

  // A real extra ownership registration must fail the same cost assertion.
  const extra = new WeakMap();
  extra.set(payload, bytes);
  assert.throws(() => assert.equal(storageCalls(spies, payload), baseline), assert.AssertionError);
});

test('encoded payload copy and borrowed view retain their public ownership contracts', () => {
  const backing = Uint8Array.from([0, 1, 2, 3, 4]);
  const input = backing.subarray(1, 4);
  const copied = ZLinkEncodedPayload.from(input);
  const owned = ZLinkEncodedPayload.fromOwned(input);
  input[0] = 9;
  assert.deepEqual(copied.data(), Uint8Array.from([1, 2, 3]));
  assert.deepEqual(owned.data(), Uint8Array.from([9, 2, 3]));
  const view = owned.view();
  assert.strictEqual(view.buffer, backing.buffer);
  assert.equal(view.byteOffset, input.byteOffset);
  assert.equal(view.byteLength, input.byteLength);
  const returned = owned.data();
  returned[0] = 7;
  assert.equal(owned.view()[0], 9);
  const clone = owned.copy();
  input[1] = 8;
  owned.close();
  assert.deepEqual(clone.data(), Uint8Array.from([9, 2, 3]));
  assert.deepEqual(owned.data(), Uint8Array.from([9, 8, 3]));
  assert.equal(borrowEncodedPayload({ data: () => backing }), undefined);
});
