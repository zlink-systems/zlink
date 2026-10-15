'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');

const correlation = '00112233445566778899aabbccddeeff';
const deadline = Date.parse('2026-09-10T00:00:00.000Z');

function bytes(part) {
  return Buffer.isBuffer(part) ? part : part.data();
}

test('outbound binary channel header writes escaped UTF-8 without a whole JSON string', t => {
  let stringifyCalls = 0;
  t.mock.method(JSON, 'stringify', () => {
    stringifyCalls += 1;
    throw new Error('channel header must not stringify a temporary object');
  });

  const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
    1,
    'ch"\n\\\ud800',
    'M😀\udc00',
    Buffer.from('00ff80', 'hex'),
    deadline,
    undefined,
    undefined,
    correlation,
    false
  );
  try {
    assert.equal(stringifyCalls, 0);
    assert.deepEqual(
      bytes(parts[0]),
      Buffer.from('{"formatMarker":242,"kind":1,"channelName":"ch\\"\\n\\\\\\ud800","messageName":"M😀\\udc00","contentType":"application/octet-stream","correlationId":"00112233445566778899aabbccddeeff","deadline":"2026-09-10T00:00:00.000Z","topic":null,"errorCode":null,"errorMessage":null,"metadata":{}}')
    );
  } finally {
    envelope.closeMessages(parts);
  }
});

// Spec 08 E1: a send or request with a flow uses the prepared header with the
// flow written into its fixed slot, byte-for-byte equal to the full writer.
test('prepared channel headers carry the flow fields like the full header writer', () => {
  const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
  const codecs = { serializers: new Map() };
  const decodeHeader = (parts) => envelope.decodeChannelHeader([{ data: () => bytes(parts[0]) }]);
  const flow = { flowId: '01920000-0000-7abc-8def-0123456789ab', flowOrigin: 'Application' };
  for (const [kind, correlationId, at] of [
    [1, correlation, deadline],
    [3, undefined, undefined]
  ]) {
    const encode = (registry) =>
      flowContext.runWithFlow(flow, () =>
        envelope.encodeChannelEnvelopePartsAtDeadline(
          kind,
          'bench',
          'BenchPayload',
          Buffer.from('payload'),
          at,
          undefined,
          registry,
          correlationId
        )
      );
    const prepared = encode(codecs);
    const written = encode(undefined);
    try {
      assert.deepEqual(bytes(prepared[0]), bytes(written[0]));
      const header = decodeHeader(prepared);
      assert.equal(header.flowId, flow.flowId);
      assert.equal(header.flowOrigin, flow.flowOrigin);
      // A second flow reuses the same prepared bytes with only its own flow id.
      const other = flowContext.runWithFlow(
        { flowId: '01920000-0000-7abc-8def-ffffffffffff', flowOrigin: 'Application' },
        () =>
          envelope.encodeChannelEnvelopePartsAtDeadline(
            kind,
            'bench',
            'BenchPayload',
            Buffer.from('payload'),
            at,
            undefined,
            codecs,
            correlationId
          )
      );
      assert.equal(decodeHeader(other).flowId, '01920000-0000-7abc-8def-ffffffffffff');
      assert.equal(decodeHeader(prepared).flowId, flow.flowId);
      envelope.closeMessages(other);
    } finally {
      envelope.closeMessages(prepared);
      envelope.closeMessages(written);
    }
  }
});

// Spec 08 I3: the inbound header is read once. A canonical ASCII header
// decodes each field from its bytes once, without a UTF-8 rescan or a
// JSON.parse per string; other headers keep the full validation.
test('canonical inbound headers decode each field once and keep UTF-8 validation for the rest', t => {
  const bufferModule = require('node:buffer');
  const flow = '01920000-0000-7abc-8def-0123456789ab';
  const ascii = Buffer.from(
    `{"formatMarker":242,"kind":1,"channelName":"bench","messageName":"BenchPayload","contentType":"application/octet-stream","correlationId":"${correlation}","deadline":"2026-09-10T00:00:00.000Z","topic":null,"errorCode":null,"errorMessage":null,"metadata":{},"flowId":"${flow}","flowOrigin":3}`
  );
  const decode = (bytes) => envelope.decodeChannelHeader([{ data: () => bytes }]);
  let utf8Checks = 0;
  let jsonParses = 0;
  const isUtf8 = bufferModule.isUtf8;
  t.mock.method(bufferModule, 'isUtf8', (value) => {
    utf8Checks += 1;
    return isUtf8(value);
  });
  const parse = JSON.parse;
  t.mock.method(JSON, 'parse', (...args) => {
    jsonParses += 1;
    return parse(...args);
  });
  const header = decode(ascii);
  assert.equal(header.channelName, 'bench');
  assert.equal(header.messageName, 'BenchPayload');
  assert.equal(header.correlationId, correlation);
  assert.equal(header.flowId, flow);
  assert.equal(utf8Checks, 0);
  assert.equal(jsonParses, 0);

  const escaped = decode(Buffer.from(ascii.toString().replace('"bench"', '"be\\"nch"')));
  assert.equal(escaped.channelName, 'be"nch');
  const unicode = decode(Buffer.from(ascii.toString().replace('"bench"', '"bénch"')));
  assert.equal(unicode.channelName, 'bénch');
  assert.ok(utf8Checks >= 1);
  const invalid = Buffer.from(ascii.toString().replace('"bench"', '"b?nch"'));
  invalid[invalid.indexOf('b?nch') + 1] = 0xff;
  assert.throws(() => decode(invalid), /valid UTF-8/);
});
