'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { encodeChannelHeader } = require('./helpers/protobuf-channel-header');

function parts(value) {
  const bytes = Buffer.isBuffer(value) ? value : encodeChannelHeader(value);
  return [{ data() { return bytes; } }];
}

function header(overrides = {}) {
  return {
    kind: 1,
    channelName: 'channel',
    messageName: 'Packet',
    contentType: 'application/json',
    correlationId: 'request-1',
    metadata: {},
    ...overrides
  };
}

test('protobuf header decodes required, optional, and repeated metadata fields', () => {
  const decoded = envelope.decodeChannelHeader(parts(header({
    deadline: '2026-09-10T00:00:00.000Z',
    topic: 'topic',
    source: 'source',
    metadata: [{ key: 'z', value: 'last' }, { key: 'a', value: 'first' }]
  })));
  assert.equal(decoded.kind, 1);
  assert.equal(decoded.channelName, 'channel');
  assert.equal(decoded.messageName, 'Packet');
  assert.equal(decoded.contentType, 'application/json');
  assert.equal(decoded.correlationId, 'request-1');
  assert.equal(decoded.deadline, '2026-09-10T00:00:00.000Z');
  assert.equal(decoded.topic, 'topic');
  assert.equal(decoded.source, 'source');
  assert.deepEqual({ ...decoded.metadata }, { z: 'last', a: 'first' });
});

test('protobuf header skips unknown fields and rejects malformed wire bytes', () => {
  const base = encodeChannelHeader(header());
  const unknown = Buffer.concat([base, Buffer.from([0x72, 0x03, 0x01, 0x02, 0x03])]);
  assert.equal(envelope.decodeChannelHeader(parts(unknown)).channelName, 'channel');
  assert.throws(() => envelope.decodeChannelHeader(parts(Buffer.from([0xff]))), /protobuf is malformed/);
  assert.throws(() => envelope.decodeChannelHeader(parts(base.subarray(0, base.length - 1))), /protobuf is malformed/);
  const invalidUtf8 = Buffer.from(base);
  const channelName = invalidUtf8.indexOf(Buffer.from('channel'));
  invalidUtf8[channelName] = 0xff;
  assert.throws(() => envelope.decodeChannelHeader(parts(invalidUtf8)), /protobuf is malformed/);
});

test('protobuf header semantic validation rejects missing and unsupported fields', () => {
  assert.throws(() => envelope.decodeChannelHeader(parts(header({ kind: 0 })), false), /kind/);
  assert.throws(() => envelope.decodeChannelHeader(parts(header({ kind: 99 })), false), /kind/);
  assert.equal(envelope.decodeChannelHeader(parts(header({ channelName: '' })), false).channelName, '');
  assert.equal(envelope.decodeChannelHeader(parts(header({ messageName: '' })), false).messageName, '');
  assert.equal(envelope.decodeChannelHeader(parts(header({ contentType: '' })), false).contentType, '');
  assert.equal(
    envelope.decodeChannelHeader(parts(header({ correlationId: null })), false).correlationId,
    null
  );
  assert.throws(() => envelope.decodeChannelHeader(parts(header({ flowId: 'invalid', flowOrigin: 99 }))), /flowId/);
  assert.throws(() => envelope.decodeChannelHeader(parts(header({ metadata: [{ key: 'x', value: 'first' }, { key: 'x', value: 'last' }] })), false), /duplicate/);
});

test('protobuf correlation validation preserves ASCII values through the 64-byte boundary', () => {
  for (const correlationId of ['\x01', '\x7f', 'a'.repeat(64)]) {
    assert.equal(
      envelope.decodeChannelHeader(parts(header({ correlationId })), false).correlationId,
      correlationId
    );
  }
  for (const correlationId of ['', 'a'.repeat(65), 'é', '\0']) {
    assert.throws(
      () => envelope.decodeChannelHeader(parts(header({ correlationId })), false),
      /correlationId/
    );
  }
});

test('tracing Off ignores malformed optional flow fields but retains correlation', () => {
  const encoded = parts(header({ flowId: 'invalid', flowOrigin: 99 }));
  const decoded = envelope.decodeChannelHeader(encoded, false);
  assert.equal(decoded.flowId, undefined);
  assert.equal(decoded.flowOrigin, undefined);
  assert.equal(decoded.correlationId, 'request-1');
});
