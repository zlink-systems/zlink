const assert = require('node:assert/strict');
const test = require('node:test');
const {
  encodeDynamicValue,
  decodeDynamicValue
} = require('../../packages/framework-codec-protobuf/dist/dynamic-value-wire');

test('DynamicValue varints reach field concat without an intermediate Buffer.from copy', (t) => {
  const original = Buffer.from;
  let copies = 0;
  t.mock.method(Buffer, 'from', function (...args) {
    if (new Error().stack.includes('at encodeVarint (')) copies++;
    return original.apply(this, args);
  });
  for (let i = 0; i < 1000; i++) {
    assert.equal(decodeDynamicValue(encodeDynamicValue(42)), 42);
  }
  assert.equal(copies, 0);
});

test('DynamicValue preserves number bytes and length-delimited varint boundaries', () => {
  assert.equal(encodeDynamicValue(42).toString('hex'), '0802190000000000004540');
  for (const value of [0, 127, 128, 2 ** 32, Number.MAX_SAFE_INTEGER, -1, -128]) {
    const expected = Buffer.from('0802190000000000000000', 'hex');
    expected.writeDoubleLE(value, 3);
    assert.deepEqual(encodeDynamicValue(value), expected);
  }
  for (const [length, prefix] of [
    [0, '08032200'],
    [127, '0803227f'],
    [128, '0803228001']
  ]) {
    const value = 'a'.repeat(length);
    const encoded = encodeDynamicValue(value);
    assert.deepEqual(encoded, Buffer.concat([Buffer.from(prefix, 'hex'), Buffer.from(value)]));
    assert.equal(decodeDynamicValue(encoded), value);
  }
});

test('DynamicValue nested encodes and decoded bytes keep independent ownership', () => {
  const bytes = Buffer.from([0, 127, 128, 255]);
  const value = { ['k'.repeat(128)]: [42, -42, { bytes }, [], {}] };
  const first = encodeDynamicValue(value);
  const second = encodeDynamicValue(value);
  const expected = Buffer.from(second);
  assert.deepEqual(first, second);
  const decoded = decodeDynamicValue(first);
  assert.deepEqual(decoded, value);
  first.fill(0);
  assert.deepEqual(second, expected);
  assert.deepEqual(decoded, value);
  bytes.fill(1);
  assert.deepEqual(second, expected);
  assert.deepEqual(decoded['k'.repeat(128)][2].bytes, Buffer.from([0, 127, 128, 255]));
});
