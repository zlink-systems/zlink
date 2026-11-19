'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  decodeChannelHeader,
  encodeChannelEnvelopeParts,
  encodeChannelReplyParts,
  isValidChannelCorrelationId
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { encodeChannelHeader: encodeHeader } = require('./helpers/protobuf-channel-header');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');

function wire(overrides = {}) {
  return encodeHeader({
    kind: 2,
    channelName: 'bench',
    messageName: 'Reply',
    contentType: 'application/json',
    correlationId: '00112233445566778899aabbccddeeff',
    metadata: {},
    ...overrides
  });
}

function decode(bytes, flowEnabled = false) {
  return decodeChannelHeader([ZLinkBufferMessage.fromOwned(bytes)], flowEnabled);
}

test('generated protobuf decoder preserves string boundaries and Unicode', () => {
  for (let count = 0; count <= 16; count += 1) {
    for (const suffix of ['', '"', '"tail', '\\"tail', '\n\t', 'é한글😀']) {
      const value = 'prefix' + '\\'.repeat(count) + suffix;
      const header = decode(wire({ channelName: value, messageName: value, topic: value }));
      assert.equal(header.channelName, value);
      assert.equal(header.messageName, value);
      assert.equal(header.topic, value);
    }
  }
});

test('generated protobuf decoder accepts control values except NUL and rejects invalid UTF-8', () => {
  for (let code = 1; code < 128; code += 1) {
    const value = 'before' + String.fromCharCode(code) + 'after';
    assert.equal(decode(wire({ channelName: value })).channelName, value);
  }
  assert.throws(() => decode(wire({ channelName: 'before\0after' })), /NUL/);
  const invalid = wire({ channelName: 'replace-me' });
  invalid[invalid.indexOf(Buffer.from('replace-me'))] = 0xff;
  assert.throws(() => decode(invalid));
});

test('correlation validation applies ASCII, NUL, and 64-byte limits', () => {
  for (let code = 1; code < 128; code += 1) {
    const value = String.fromCharCode(code).repeat(64);
    assert.equal(isValidChannelCorrelationId(value), true);
    assert.equal(decode(wire({ correlationId: value })).correlationId, value);
  }
  for (const value of ['', 'x'.repeat(65), 'é', '\0', '\ud800', '😀']) {
    assert.equal(isValidChannelCorrelationId(value), false);
    assert.throws(() => decode(wire({ correlationId: value })));
  }
});

test('generated request and reply headers preserve correlation, deadlines, and flow fields', () => {
  const codecs = { serializers: new Map() };
  const payload = Buffer.from('payload');
  const ids = new Set();
  const headerOf = (parts, flow = false) => decode(parts[0], flow);
  for (let index = 0; index < 12; index += 1) {
    const requestParts = encodeChannelEnvelopeParts(1, 'bench', 'Packet', payload, 1000 + index, undefined, codecs, undefined, false);
    const request = headerOf(requestParts);
    assert.equal(isValidChannelCorrelationId(request.correlationId), true);
    assert.equal(request.correlationId.length, 32);
    assert.equal(ids.has(request.correlationId), false);
    ids.add(request.correlationId);
    assert.equal(typeof request.deadline, 'string');
    for (const origin of [undefined, 'Inbound', 'Timer', 'Application', 'Lifecycle']) {
      const withFlow = origin === undefined ? request : {
        ...request,
        flowId: '01920000-0000-7abc-8def-0123456789ab',
        flowOrigin: origin
      };
      const reply = headerOf(encodeChannelReplyParts(withFlow, payload, codecs), true);
      assert.equal(reply.kind, 2);
      assert.equal(reply.correlationId, request.correlationId);
      assert.equal(reply.deadline, null);
      assert.equal(reply.flowId, withFlow.flowId);
      assert.equal(reply.flowOrigin, origin);
    }
    const command = headerOf(encodeChannelEnvelopeParts(3, 'bench', 'Packet', payload, undefined, undefined, codecs, undefined, false));
    assert.equal(command.kind, 3);
    assert.equal(command.correlationId, null);
    assert.equal(command.deadline, null);
  }
});
