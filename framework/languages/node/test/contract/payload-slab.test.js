'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { allocatePayloadBytes } = require('../../packages/framework/dist/contracts/Common/payload-slab');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');
const { decodeFrameworkTypedPayloadMessage } = require('../../packages/framework/dist/runtime/messaging/payload-codec');

function startSlab() {
  let bytes;
  do { bytes = allocatePayloadBytes(16384); } while (bytes.byteOffset !== 0);
  return bytes;
}

test('payload slab aligns independent views to eight bytes', () => {
  const first = startSlab();
  first.fill(0x11);
  const one = allocatePayloadBytes(1);
  const nine = allocatePayloadBytes(9);
  const next = allocatePayloadBytes(1);
  assert.equal(one.buffer, first.buffer);
  assert.equal(nine.buffer, first.buffer);
  assert.equal(next.buffer, first.buffer);
  assert.equal(one.byteOffset, 16384);
  assert.equal(nine.byteOffset, one.byteOffset + 8);
  assert.equal(next.byteOffset, nine.byteOffset + 16);
  nine.fill(0x22);
  one.fill(0x33);
  next.fill(0x44);
  assert.ok(first.every(value => value === 0x11));
  assert.ok(nine.every(value => value === 0x22));
  assert.equal(one[0], 0x33);
  assert.equal(next[0], 0x44);
  const empty = allocatePayloadBytes(0);
  assert.equal(empty.byteLength, 0);
  assert.equal(empty.byteOffset % 8, 0);
});

test('payload slab includes the 16 KiB cap, allocates larger views directly, and changes slab on overflow', () => {
  const first = startSlab();
  assert.equal(first.buffer.byteLength, 65536);
  const direct = allocatePayloadBytes(16385);
  const large = allocatePayloadBytes(70000);
  assert.equal(direct.buffer.byteLength, 16385);
  assert.equal(large.buffer.byteLength, 70000);
  assert.equal(direct.byteOffset, 0);
  assert.equal(large.byteOffset, 0);
  assert.notEqual(direct.buffer, first.buffer);
  for (let index = 1; index < 4; index++) {
    const bytes = allocatePayloadBytes(16384);
    assert.equal(bytes.buffer, first.buffer);
    assert.equal(bytes.byteOffset, index * 16384);
    bytes.fill(index);
  }
  const overflow = allocatePayloadBytes(1);
  assert.notEqual(overflow.buffer, first.buffer);
  assert.equal(overflow.buffer.byteLength, 65536);
  assert.equal(overflow.byteOffset, 0);
  overflow.fill(0xff);
  assert.ok(new Uint8Array(first.buffer, 16384, 16384).every(value => value === 1));
});

test('payload allocation retains Buffer errors for invalid lengths', () => {
  assert.throws(() => allocatePayloadBytes(-1), { code: 'ERR_OUT_OF_RANGE' });
  assert.throws(() => allocatePayloadBytes(Infinity), { code: 'ERR_OUT_OF_RANGE' });
  assert.throws(() => allocatePayloadBytes('4'), { code: 'ERR_INVALID_ARG_TYPE' });
});

test('4 KiB typed decode owns its copy after receive storage and subsequent slab views change', () => {
  startSlab();
  const receiveStorage = new Uint8Array(4098).fill(0x5a);
  const input = receiveStorage.subarray(1, 4097);
  const message = ZLinkBufferMessage.fromOwned(input);
  class BinaryValue {}
  const contentType = 'application/x-ownership-test';
  const registry = new Map([[contentType, { deserialize: payload => payload.data() }]]);
  const retained = decodeFrameworkTypedPayloadMessage(message, registry, BinaryValue, contentType);
  assert.equal(retained.constructor, Uint8Array);
  assert.equal(Buffer.isBuffer(retained), false);
  assert.equal(retained.byteLength, 4096);
  assert.notEqual(retained.buffer, receiveStorage.buffer);
  assert.equal(retained.buffer.byteLength, 65536);
  const subsequent = allocatePayloadBytes(4096);
  assert.equal(subsequent.buffer, retained.buffer);
  assert.ok(subsequent.byteOffset >= retained.byteOffset + retained.byteLength);
  input.fill(0x11);
  subsequent.fill(0x22);
  message.close();
  assert.ok(retained.every(value => value === 0x5a));
  retained.fill(0x33);
  assert.ok(input.every(value => value === 0x11));
  assert.ok(subsequent.every(value => value === 0x22));
});
