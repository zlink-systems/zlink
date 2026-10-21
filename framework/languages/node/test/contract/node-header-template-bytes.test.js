'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  ZLinkEncodedPayload
} = require('../../packages/framework/dist/contracts/Common/ZLinkEncodedPayload');
const { ZLinkMessage } = require('../../packages/framework/dist/contracts/Common/ZLinkMessage');

test('cold and warm prepared headers match the full writer for every flow origin', () => {
  const codec = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  const flowContext = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
  const payload = ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(Buffer.from('body')));
  const correlationId = '00112233445566778899aabbccddeeff';
  const deadline = 2_000_000_000_000;
  const flows = [
    undefined,
    { flowId: '00000000-0000-7000-8000-000000000001', flowOrigin: 'Inbound' },
    { flowId: '00000000-0000-7000-8000-000000000002', flowOrigin: 'Timer' },
    { flowId: '00000000-0000-7000-8000-000000000003', flowOrigin: 'Application' },
    { flowId: '00000000-0000-7000-8000-000000000004', flowOrigin: 'Lifecycle' }
  ];
  const bytes = (parts) => parts.map((part) => Buffer.from(part));
  const withFlow = (flow, callback) =>
    flow === undefined ? callback() : flowContext.runWithFlow(flow, callback);
  const encode = (kind, flow, codecs, deadlineUnixMs, requestCorrelationId) =>
    withFlow(flow, () =>
      codec.encodeChannelEnvelopePartsAtDeadline(
        kind,
        'header-bytes',
        'HeaderBytesPayload',
        payload,
        deadlineUnixMs,
        undefined,
        codecs,
        requestCorrelationId,
        flow !== undefined
      )
    );

  for (const flow of flows) {
    const codecs = { serializers: new Map() };
    for (const [label, kind] of [
      ['command', 3],
      ['request', 1]
    ]) {
      const deadlineUnixMs = kind === 1 ? deadline : undefined;
      const requestCorrelationId = kind === 1 ? correlationId : undefined;
      const fullWriter = bytes(encode(kind, flow, undefined, deadlineUnixMs, requestCorrelationId));
      const cold = bytes(encode(kind, flow, codecs, deadlineUnixMs, requestCorrelationId));
      const warm = bytes(encode(kind, flow, codecs, deadlineUnixMs, requestCorrelationId));
      assert.deepEqual(cold, fullWriter, `${label} cold header (${flow?.flowOrigin ?? 'no-flow'})`);
      assert.deepEqual(warm, fullWriter, `${label} warm header (${flow?.flowOrigin ?? 'no-flow'})`);
    }

    for (const [label, kind, deadlineUnixMs, requestCorrelationId] of [
      ['command with deadline', 3, deadline, undefined],
      ['request without deadline', 1, undefined, correlationId]
    ]) {
      const fullWriter = bytes(encode(kind, flow, undefined, deadlineUnixMs, requestCorrelationId));
      const firstFallback = bytes(encode(kind, flow, codecs, deadlineUnixMs, requestCorrelationId));
      const secondFallback = bytes(
        encode(kind, flow, codecs, deadlineUnixMs, requestCorrelationId)
      );
      assert.deepEqual(
        firstFallback,
        fullWriter,
        `${label} first fallback (${flow?.flowOrigin ?? 'no-flow'})`
      );
      assert.deepEqual(
        secondFallback,
        fullWriter,
        `${label} repeated fallback (${flow?.flowOrigin ?? 'no-flow'})`
      );
    }

    const requestParts = encode(1, flow, codecs, deadline, correlationId);
    const request = codec.decodeChannelHeader(
      requestParts.map((part) => ZLinkBufferMessage.fromOwned(part))
    );
    const fullWriterReply = bytes(codec.encodeChannelReplyParts(request, payload));
    const coldReply = bytes(codec.encodeChannelReplyParts(request, payload, codecs));
    const warmReply = bytes(codec.encodeChannelReplyParts(request, payload, codecs));
    assert.deepEqual(
      coldReply,
      fullWriterReply,
      `response cold header (${flow?.flowOrigin ?? 'no-flow'})`
    );
    assert.deepEqual(
      warmReply,
      fullWriterReply,
      `response warm header (${flow?.flowOrigin ?? 'no-flow'})`
    );
  }
});
