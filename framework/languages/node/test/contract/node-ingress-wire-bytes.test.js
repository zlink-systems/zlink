'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { runWithFlow } = require('../../packages/framework/dist/runtime/diagnostics/flow-context');

// Literal byte fixtures precede ingress changes. Do not regenerate expectations
// from either encoder: a matching encode/decode bug must still fail this file.
const correlation = '00112233445566778899aabbccddeeff';
const flow = { flowId: '01992176-0000-7000-8000-000000000001', flowOrigin: 'Application' };
const deadline = Date.parse('2026-09-10T00:00:00.000Z');
const requestHeader = '{"formatMarker":242,"kind":1,"channelName":"echo","messageName":"Echo","contentType":"application/octet-stream","correlationId":"00112233445566778899aabbccddeeff","deadline":"2026-09-10T00:00:00.000Z","topic":null,"errorCode":null,"errorMessage":null,"metadata":{}}';

function bytes(part) {
  return Buffer.isBuffer(part) ? part : part.data();
}

function messageParts(parts) {
  return parts.map(part => ({ data: () => bytes(part) }));
}

test('ingress wire golden: node/channel routing headers and terminal reply', () => {
  const id = 0x0102030405060708n;
  assert.equal(wire.encodeNodeSendHeader().toString('hex'), '5a4d011000');
  assert.equal(wire.encodeNodeRequestHeader(id).toString('hex'), '5a4d0111000102030405060708');
  assert.equal(wire.encodeChannelSendHeader('echo').toString('hex'), '5a4d011200046563686f');
  assert.equal(wire.encodeChannelRequestHeader(id, 'echo').toString('hex'), '5a4d0113000102030405060708046563686f');
  assert.equal(wire.encodeReplyHeader(id).toString('hex'), '5a4d01140001020304050607080000000000000000');
  assert.equal(wire.encodeReplyHeader(id, 104, 16).toString('hex'), '5a4d01140001020304050607080000006800000010');
});

test('ingress wire golden: multipart packing and direct packing produce identical bytes', () => {
  // Version, body length, text8 packet/content type, payload length, then
  // count + length-prefixed parts. Includes empty and offset binary parts.
  const source = Buffer.from('aabb00ff80cc', 'hex');
  const parts = [source.subarray(2, 5), Buffer.alloc(0), Buffer.from('가')];
  const packed = Buffer.from('000000030000000300ff800000000000000003eab080', 'hex');
  const expected = Buffer.from('010000001e0150016300000016000000030000000300ff800000000000000003eab080', 'hex');
  const legacy = wire.encodeApplicationPayload({ packetName: 'P', contentType: 'c', payload: packed });
  const direct = wire.encodeMultipartApplicationPayload(parts, 'P', 'c');
  assert.deepEqual(legacy, expected);
  assert.deepEqual(direct, expected);
  const decoded = wire.decodeApplicationPayloadView(expected);
  assert.equal(decoded.packetName, 'P');
  assert.equal(decoded.contentType, 'c');
  assert.deepEqual(decoded.payload, packed);
  assert.equal(decoded.payload.buffer, expected.buffer);
  assert.equal(decoded.payload.byteOffset, expected.byteOffset + 13);
  source.fill(0);
  assert.deepEqual(direct, expected, 'outbound frame owns its bytes after encode returns');
});

test('ingress wire golden: empty application payload and malformed lengths', () => {
  const expected = Buffer.from('01000000080150016300000000', 'hex');
  assert.deepEqual(wire.encodeApplicationPayload({ packetName: 'P', contentType: 'c', payload: Buffer.alloc(0) }), expected);
  assert.equal(wire.decodeApplicationPayloadView(expected).payload.length, 0);
  const trailing = Buffer.concat([expected, Buffer.from([0])]);
  assert.throws(() => wire.decodeApplicationPayloadView(trailing), /length mismatch/);
  assert.throws(() => wire.decodeApplicationPayloadView(expected.subarray(0, 12)));
});

for (const size of [1024, 4096]) {
  test(`ingress wire: every byte value survives ${size}-byte offset payload and multipart framing`, () => {
    const backing = Buffer.alloc(size + 2, 0xee);
    const payload = backing.subarray(1, size + 1);
    for (let i = 0; i < size; i++) payload[i] = i & 0xff;
    const packed = Buffer.alloc(8 + size);
    packed.writeUInt32BE(1, 0);
    packed.writeUInt32BE(size, 4);
    payload.copy(packed, 8);
    const frame = wire.encodeMultipartApplicationPayload([payload], 'P', 'c');
    assert.equal(frame[0], 1);
    assert.equal(frame.readUInt32BE(1), 16 + size);
    assert.equal(frame.readUInt32BE(9), 8 + size);
    assert.deepEqual(frame.subarray(13), packed);
    assert.deepEqual(frame, wire.encodeApplicationPayload({ packetName: 'P', contentType: 'c', payload: packed }));
    assert.deepEqual(wire.decodeApplicationPayloadView(frame).payload.subarray(8), payload);
  });
}

test('ingress wire golden: channel request header ordering and binary body with tracing Off', () => {
  const body = Buffer.from('00ff80', 'hex');
  const parts = envelope.encodeChannelEnvelopePartsAtDeadline(1, 'echo', 'Echo', body, deadline, undefined, undefined, correlation, false);
  try {
    assert.equal(parts.length, 2);
    assert.deepEqual(bytes(parts[0]), Buffer.from(requestHeader));
    assert.deepEqual(bytes(parts[1]), body);
    const header = envelope.decodeChannelHeader(messageParts(parts), false);
    assert.equal(header.correlationId, correlation);
    assert.equal(header.flowId, undefined);
  } finally {
    envelope.closeMessages(parts);
  }
});

test('ingress wire golden: traced request, reply, error, and metadata retain the existing field layout', () => {
  const parts = runWithFlow(flow, () => envelope.encodeChannelEnvelopePartsAtDeadline(
    1, 'echo', 'Echo', Buffer.from('00ff80', 'hex'), deadline,
    undefined, undefined, correlation, true, new Map([['trace', '가']])
  ));
  let reply;
  let error;
  try {
    const expectedRequest = '{"formatMarker":242,"kind":1,"channelName":"echo","messageName":"Echo","contentType":"application/octet-stream","correlationId":"00112233445566778899aabbccddeeff","deadline":"2026-09-10T00:00:00.000Z","topic":null,"errorCode":null,"errorMessage":null,"metadata":{"trace":"가"},"flowId":"01992176-0000-7000-8000-000000000001","flowOrigin":3}';
    assert.deepEqual(bytes(parts[0]), Buffer.from(expectedRequest));
    const header = envelope.decodeChannelHeader(messageParts(parts));
    assert.equal(header.flowOrigin, 'Application');
    assert.equal(header.metadata.trace, '가');
    reply = envelope.encodeChannelReplyParts(header, Buffer.from('80ff00', 'hex'));
    assert.deepEqual(bytes(reply[0]), Buffer.from('{"formatMarker":242,"kind":2,"channelName":"echo","messageName":"Echo","contentType":"application/octet-stream","correlationId":"00112233445566778899aabbccddeeff","deadline":null,"topic":null,"metadata":{},"flowId":"01992176-0000-7000-8000-000000000001","flowOrigin":3}'));
    assert.deepEqual(bytes(reply[1]), Buffer.from('80ff00', 'hex'));
    error = envelope.encodeChannelErrorReplyParts(header, new Error('bad "value"'));
    assert.deepEqual(bytes(error[0]), Buffer.from('{"formatMarker":242,"kind":5,"channelName":"echo","messageName":"Echo","contentType":"application/json","correlationId":"00112233445566778899aabbccddeeff","deadline":null,"topic":null,"errorCode":"internal_failure","errorMessage":"bad \\"value\\"","metadata":{},"flowId":"01992176-0000-7000-8000-000000000001","flowOrigin":3}'));
    assert.deepEqual(bytes(error[1]), Buffer.from('null'));
  } finally {
    envelope.closeMessages(parts);
    if (reply) envelope.closeMessages(reply);
    if (error) envelope.closeMessages(error);
  }
});
