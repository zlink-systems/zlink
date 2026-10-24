'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const channel = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { frameworkPair } = require('../../perf/request-backpressure.cjs');

// Observe every Framework module, so moving normalization to another helper
// still fails. The binding terminal is a separate, permitted owner.
function trackStringBuffers(t) {
  const from = Buffer.from;
  const sites = [];
  let binding = 0;
  let strings = 0;
  const stackLimit = Error.stackTraceLimit;
  Error.stackTraceLimit = 60;
  t.after(() => { Error.stackTraceLimit = stackLimit; });
  t.mock.method(Buffer, 'from', function(input, ...args) {
    if (typeof input === 'string') {
      strings++;
      const stack = new Error().stack.replaceAll('\\', '/');
      const owner = stack.split('\n').find(line =>
        line.includes('/node_modules/@zlink-systems/zlink/') || line.includes('/packages/framework/dist/'));
      if (owner?.includes('/node_modules/@zlink-systems/zlink/')) binding++;
      else if (owner) sites.push(stack);
    }
    return from.call(Buffer, input, ...args);
  });
  return { sites, binding: () => binding, strings: () => strings };
}

test('mixed multipart strings have the exact UTF-8 wire bytes without intermediate Buffers', t => {
  const parts = [Buffer.from([0, 255]), '', 'ASCII', '한글😀', '\ud800', '\udc00', 'a\ud800b', '"\\\n', new Uint8Array([1, 2])];
  const bytes = parts.map(part => typeof part === 'string' ? Buffer.from(part) : part);
  const expected = wire.encodeMultipartApplicationPayload(bytes, 'packet', 'application/test');
  const count = trackStringBuffers(t);
  const allocations = t.mock.method(Buffer, 'allocUnsafe');
  const actual = wire.encodeMultipartApplicationPayload(parts, 'packet', 'application/test');
  assert.equal(allocations.mock.callCount(), 1, 'one final application frame');
  assert.deepEqual(actual, expected);
  assert.equal(count.sites.length, 0, count.sites.join('\n'));
  const payload = wire.decodeApplicationPayloadView(actual).payload;
  assert.equal(payload.readUInt32BE(0), parts.length);
  let offset = 4;
  for (const part of bytes) {
    const length = payload.readUInt32BE(offset);
    assert.equal(length, part.byteLength);
    offset += 4;
    assert.deepEqual(payload.subarray(offset, offset + length), Buffer.from(part));
    offset += length;
  }
  assert.equal(offset, payload.length);
});

test('JSON parts snapshot at serialization while binary parts keep their identity', t => {
  const value = { text: '한글😀', nested: { value: 1 } };
  const expected = JSON.stringify(value);
  const count = trackStringBuffers(t);
  const encode = input => channel.encodeChannelEnvelopePartsAtDeadline(
    1, 'codec', 'Packet', input, 2000000000000, undefined,
    { serializers: new Map() }, '00112233445566778899aabbccddeeff', false
  );
  const first = encode(value);
  value.nested.value = 2;
  const second = encode(value);
  assert.equal(String(first[1]), expected);
  assert.equal(String(second[1]), JSON.stringify(value));
  const binary = Buffer.from([1, 2, 255]);
  assert.equal(encode(binary)[1], binary);
  const array = new Uint8Array([3, 4]);
  assert.equal(encode(array)[1], array);
  const circular = {}; circular.self = circular;
  assert.throws(() => encode(circular));
  assert.equal(count.sites.length, 0, count.sites.join('\n'));
});

test('typed remote request and reply create no Framework string Buffers', async t => {
  const pair = await frameworkPair();
  try {
    for (let i = 0; i < 5; i++) pair.validate(await pair.submit().reply);
    const count = trackStringBuffers(t);
    for (let i = 0; i < 20; i++) pair.validate(await pair.submit().reply);
    t.diagnostic(JSON.stringify({ frameworkStringBuffers: count.sites.length, bindingStringBuffers: count.binding(), requests: 20 }));
    assert.equal(count.sites.length, 0, count.sites.slice(0, 2).join('\n'));
  } finally { await pair.close(); }
});

test('Buffer wrapper detects copies and distinguishes shared views', t => {
  const count = trackStringBuffers(t);
  // The wrapper must observe a real Buffer.from call regardless of its owner.
  const spy = t.mock.method(Buffer, 'allocUnsafe');
  const original = Buffer.from([1, 2]);
  const copy = Buffer.from(original);
  const view = Buffer.from(original.buffer, original.byteOffset, original.byteLength);
  original[0] = 9;
  assert.equal(copy[0], 1);
  assert.equal(view[0], 9);
  assert.equal(Buffer.from('한😀').length, 7);
  assert.equal(count.strings(), 1);
  assert.throws(() => assert.equal(count.strings(), 0));
  Buffer.allocUnsafe(1);
  assert.equal(spy.mock.callCount(), 1);
  assert.equal(count.sites.length, 0);
  assert.throws(() => assert.equal(copy[0], original[0]));
});


test('bare multipart uses the same UTF-8 writer as application framing', t => {
  const parts = ['한😀', '', '\ud800', new Uint8Array([0, 255])];
  const expected = wire.decodeApplicationPayloadView(wire.encodeMultipartApplicationPayload(
    parts.map(part => typeof part === 'string' ? Buffer.from(part) : part), 'p', 'c'
  )).payload;
  const count = trackStringBuffers(t);
  assert.deepEqual(wire.encodeMultipartPayload(parts), expected);
  assert.equal(count.sites.length, 0);
  assert.throws(() => wire.encodeMultipartPayload([]));
  assert.throws(() => wire.encodeMultipartApplicationPayload([], 'p', 'c'));
});

test('custom and encoded payloads retain their existing byte representation', () => {
  const framework = require('../../packages/framework/dist/internal');
  const bytes = Buffer.from([0, 255, 42]);
  let calls = 0;
  const serializer = {
    canSerialize: () => true,
    serialize() { calls++; return framework.ZLinkEncodedPayload.from(bytes); }
  };
  const parts = channel.encodeChannelEnvelopePartsAtDeadline(
    1, 'codec', 'Packet', { value: 1 }, 2000000000000, undefined,
    { serializers: new Map([['application/x-test', serializer]]) },
    '00112233445566778899aabbccddeeff', false
  );
  assert.equal(calls, 1);
  assert.deepEqual(parts[1], bytes);
  assert.equal(JSON.parse(parts[0].toString()).contentType, 'application/x-test');
  const encoded = framework.ZLinkMessage.fromEncoded(framework.ZLinkEncodedPayload.from(bytes));
  const raw = channel.encodeChannelEnvelopeParts(1, 'codec', 'Packet', encoded);
  assert.deepEqual(raw[1], bytes);
  assert.equal(calls, 1);
});
