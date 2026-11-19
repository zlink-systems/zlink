'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { encodeChannelHeader } = require('./helpers/protobuf-channel-header');

const correlation = '00112233445566778899aabbccddeeff';
const deadline = Date.parse('2026-09-10T00:00:00.000Z');

function bytes(part) {
  return Buffer.isBuffer(part) ? part : part.data();
}

test('outbound header uses the generated protobuf codec and matches literal golden bytes', t => {
  t.mock.method(JSON, 'stringify', () => assert.fail('channel header must not use JSON serialization'));
  const parts = envelope.encodeChannelEnvelopePartsAtDeadline(
    1,
    'ch"\n\\',
    'M😀',
    Buffer.from('00ff80', 'hex'),
    deadline,
    undefined,
    undefined,
    correlation,
    false
  );
  try {
    assert.equal(
      bytes(parts[0]).toString('hex'),
      '080112056368220a5c1a054df09f988022186170706c69636174696f6e2f6f637465742d73747265616d2a2030303131323233333434353536363737383839396161626263636464656566663218323032362d30392d31305430303a30303a30302e3030305a'
    );
  } finally {
    envelope.closeMessages(parts);
  }
});

test('generated channel headers carry matching flow fields across serializer registries', () => {
  const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
  const codecs = { serializers: new Map() };
  const decodeHeader = parts => envelope.decodeChannelHeader([{ data: () => bytes(parts[0]) }]);
  const flow = { flowId: '01920000-0000-7abc-8def-0123456789ab', flowOrigin: 'Application' };
  const parts = flowContext.runWithFlow(flow, () => envelope.encodeChannelEnvelopePartsAtDeadline(
    1, 'bench', 'BenchPayload', Buffer.from('payload'), deadline,
    undefined, codecs, correlation
  ));
  try {
    const header = decodeHeader(parts);
    assert.equal(header.flowId, flow.flowId);
    assert.equal(header.flowOrigin, flow.flowOrigin);
  } finally {
    envelope.closeMessages(parts);
  }
});

test('protobuf decoder validates string fields without parsing a JSON header', t => {
  const { decodeChannelHeader } = require('./helpers/protobuf-channel-header');
  const bytes = encodeChannelHeader({
    kind: 1, channelName: 'bench', messageName: 'BenchPayload',
    contentType: 'application/octet-stream', correlationId: correlation
  });
  let jsonParses = 0;
  const parse = JSON.parse;
  t.mock.method(JSON, 'parse', (...args) => { jsonParses++; return parse(...args); });
  assert.equal(envelope.decodeChannelHeader([{ data: () => bytes }]).channelName, 'bench');
  assert.equal(jsonParses, 0);
  const invalidUtf8 = Buffer.from(bytes);
  invalidUtf8[invalidUtf8.indexOf(Buffer.from('bench'))] = 0xff;
  assert.throws(() => decodeChannelHeader(invalidUtf8));
});
