const test = require('node:test');
const assert = require('node:assert/strict');
const {
  decodeChannelHeader,
  encodeChannelEnvelopeParts,
  encodeChannelReplyParts,
  isValidChannelCorrelationId
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');

function wire(overrides = {}) {
  return Buffer.from(
    JSON.stringify({
      formatMarker: 242,
      kind: 2,
      channelName: 'bench',
      messageName: 'Reply',
      contentType: 'application/json',
      correlationId: '00112233445566778899aabbccddeeff',
      deadline: null,
      topic: null,
      metadata: {},
      ...overrides
    })
  );
}

function decode(bytes, flowEnabled = false) {
  return decodeChannelHeader([ZLinkBufferMessage.fromOwned(bytes)], flowEnabled);
}

test('canonical strings preserve quote boundaries after odd and even backslash runs', () => {
  for (let count = 0; count <= 16; count += 1) {
    for (const suffix of ['', '"', '"tail', '\\"tail', '\n\t', 'é한글😀']) {
      const value = 'prefix' + '\\'.repeat(count) + suffix;
      const bytes = wire({ channelName: value, messageName: value, topic: value });
      const header = decode(bytes);
      assert.equal(header.channelName, value);
      assert.equal(header.messageName, value);
      assert.equal(header.topic, value);
    }
  }
});

test('canonical strings preserve all escaped ASCII control values in header fields', () => {
  for (let code = 0; code < 128; code += 1) {
    const value = 'before' + String.fromCharCode(code) + 'after';
    const header = decode(wire({ channelName: value, messageName: value }));
    assert.equal(header.channelName, value);
    assert.equal(header.messageName, value);
  }
});

test('canonical strings reject raw controls, invalid escapes, and incomplete boundaries', () => {
  const normal = wire().toString();
  for (let code = 0; code < 32; code += 1) {
    assert.throws(() =>
      decode(Buffer.from(normal.replace('"bench"', '"be' + String.fromCharCode(code) + 'nch"')))
    );
  }
  for (const invalid of ['"be\\xnch"', '"be\\u12xzch"', '"be\\u123"', '"be\\"']) {
    assert.throws(() => decode(Buffer.from(normal.replace('"bench"', invalid))));
  }
  const escaped = wire({ channelName: '\\"end\\\\' });
  for (let end = 0; end < escaped.length; end += 1) {
    assert.throws(() => decode(escaped.subarray(0, end)));
  }
});

test('canonical non-ASCII strings still reject invalid UTF-8 and do not retain mutable bytes', () => {
  const value = 'é한글😀';
  const bytes = wire({ channelName: value, messageName: value });
  const header = decode(bytes);
  assert.equal(header.channelName, value);
  assert.equal(header.messageName, value);
  bytes.fill(0);
  assert.equal(header.channelName, value);
  assert.equal(header.messageName, value);
  for (const bad of [[0xff], [0xc0, 0xaf], [0xed, 0xa0, 0x80], [0xf4, 0x90, 0x80, 0x80]]) {
    const invalid = wire({ channelName: 'replace-me' });
    Buffer.from(bad).copy(invalid, invalid.indexOf('replace-me'));
    assert.throws(() => decode(invalid), /UTF-8/);
  }
});

test('correlation validation preserves every ASCII value and the 64-byte boundary', () => {
  for (let code = 0; code < 128; code += 1) {
    const value = String.fromCharCode(code).repeat(64);
    assert.equal(isValidChannelCorrelationId(value), true);
    assert.equal(decode(wire({ correlationId: value })).correlationId, value);
  }
  for (const value of ['', 'x'.repeat(65), 'é', '\ud800', '😀']) {
    assert.equal(isValidChannelCorrelationId(value), false);
    assert.throws(() => decode(wire({ correlationId: value })));
  }
});

test('prepared command, request, and reply headers keep their own deadline and flow fields', () => {
  const codecs = { serializers: new Map() };
  const payload = Buffer.from('payload');
  const ids = new Set();
  const headerOf = (parts, flow = false) => decode(parts[0], flow);
  for (let index = 0; index < 12; index += 1) {
    const requestParts = encodeChannelEnvelopeParts(
      1,
      'bench',
      'Packet',
      payload,
      1000 + index,
      undefined,
      codecs,
      undefined,
      false
    );
    const request = headerOf(requestParts);
    assert.equal(isValidChannelCorrelationId(request.correlationId), true);
    assert.equal(request.correlationId.length, 32);
    assert.equal(ids.has(request.correlationId), false);
    ids.add(request.correlationId);
    assert.equal(typeof request.deadline, 'string');
    for (const origin of [undefined, 'Inbound', 'Timer', 'Application', 'Lifecycle']) {
      const withFlow =
        origin === undefined
          ? request
          : {
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
    const command = headerOf(
      encodeChannelEnvelopeParts(
        3,
        'bench',
        'Packet',
        payload,
        undefined,
        undefined,
        codecs,
        undefined,
        false
      )
    );
    assert.equal(command.kind, 3);
    assert.equal(command.correlationId, null);
    assert.equal(command.deadline, null);
  }
});
