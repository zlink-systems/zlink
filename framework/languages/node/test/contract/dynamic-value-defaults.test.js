const assert = require('node:assert/strict');
const test = require('node:test');
const {
  encodeDynamicValue,
  decodeDynamicValue
} = require('../../packages/framework-codec-protobuf/dist/dynamic-value-wire');

test('DynamicValue scalar decode does not create unused empty Buffers', () => {
  const encoded = encodeDynamicValue(42);
  const original = Buffer.alloc;
  let emptyBuffers = 0;
  Buffer.alloc = function (size, ...args) {
    if (size === 0) emptyBuffers++;
    return original(size, ...args);
  };
  try {
    assert.equal(emptyBuffers, 0);
    for (let i = 0; i < 1000; i++) Buffer.alloc(0);
    assert.equal(emptyBuffers, 1000);
    emptyBuffers = 0;
    for (let i = 0; i < 1000; i++) assert.equal(decodeDynamicValue(encoded), 42);
    assert.equal(emptyBuffers, 0);
  } finally {
    Buffer.alloc = original;
  }
});

test('DynamicValue missing fields preserve defaults and independent mutable results', () => {
  for (const [wire, expected] of [
    [[], null],
    [[8, 0], null],
    [[8, 1], false],
    [[8, 2], 0],
    [[8, 3], ''],
    [[8, 4], {}],
    [[8, 5], []],
    [[8, 6], Buffer.alloc(0)]
  ]) {
    const encoded = Buffer.from(wire);
    const first = decodeDynamicValue(encoded);
    const second = decodeDynamicValue(encoded);
    assert.deepEqual(first, expected);
    assert.deepEqual(second, expected);
    if (first !== null && typeof first === 'object') {
      assert.notEqual(first, second);
      first.marker = true;
      assert.equal(Object.hasOwn(second, 'marker'), false);
    }
    if (Buffer.isBuffer(first)) assert.notEqual(first.buffer, second.buffer);
  }
});

test('DynamicValue field order, duplicate fields and unknown fields retain their results', () => {
  for (const [wire, expected] of [
    [[58, 1, 7, 8, 6], Buffer.from([7])],
    [[8, 6, 58, 1, 7, 58, 1, 9], Buffer.from([9])],
    [[8, 6, 58, 1, 7, 58, 0], Buffer.alloc(0)],
    [
      [50, 2, 8, 1, 8, 5, 50, 2, 8, 0],
      [false, null]
    ],
    [[8, 6, 58, 1, 7, 8, 5], []],
    [[8, 5, 50, 2, 8, 1, 8, 6], Buffer.alloc(0)],
    [[8, 5, 64, 42, 74, 1, 255], []]
  ]) {
    const encoded = Buffer.from(wire);
    const before = Buffer.from(encoded);
    assert.deepEqual(decodeDynamicValue(encoded), expected);
    assert.deepEqual(decodeDynamicValue(encoded), expected);
    assert.deepEqual(encoded, before);
  }
});

test('DynamicValue nested decoded values own their storage across repeated decode and mutation', () => {
  const original = {
    primitives: [null, true, false, 42, -2.5, '', '한글'],
    nested: [{ bytes: Buffer.from([1, 2, 3]), empty: Buffer.alloc(0) }, []]
  };
  const encoded = encodeDynamicValue(original);
  const before = Buffer.from(encoded);
  const first = decodeDynamicValue(encoded);
  const second = decodeDynamicValue(encoded);
  assert.deepEqual(first, original);
  assert.deepEqual(second, original);
  assert.notEqual(first.nested, second.nested);
  assert.notEqual(first.nested[1], second.nested[1]);
  for (const key of ['bytes', 'empty']) {
    assert.notEqual(first.nested[0][key], second.nested[0][key]);
    assert.notEqual(first.nested[0][key].buffer, second.nested[0][key].buffer);
    assert.notEqual(first.nested[0][key].buffer, encoded.buffer);
    new Uint8Array(first.nested[0][key].buffer).fill(255);
  }
  first.nested[1].push('changed');
  first.primitives[0] = 'changed';
  assert.deepEqual(second, original);
  assert.deepEqual(encoded, before);
  assert.deepEqual(decodeDynamicValue(encoded), original);
});

test('DynamicValue rejects malformed fields even when the final kind does not use them', () => {
  for (const wire of [
    [8],
    [8, 7],
    [8, 0, 58, 2, 1],
    [8, 0, 50, 2, 8],
    [8, 0, 56, 1],
    [8, 0, 48, 1],
    [8, 0, 66, 2, 1],
    [8, 0, 67]
  ]) {
    const encoded = Buffer.from(wire);
    assert.throws(() => decodeDynamicValue(encoded));
    assert.throws(() => decodeDynamicValue(encoded));
  }
});
