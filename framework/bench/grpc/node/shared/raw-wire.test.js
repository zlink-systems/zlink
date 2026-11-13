// SPDX-License-Identifier: MPL-2.0
'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const protoLoader = require('@grpc/proto-loader');
const raw = require('./raw-wire');
const header = require('./bench-metric-header');
const { BenchPayload } = require('./framework-bench-contract');
const { createBenchPayloadSerializer } = require('./framework-protobuf');

const service = protoLoader.loadSync(path.join(__dirname, '../../proto/bench.proto'), {
  keepCase: true, longs: String, enums: String, defaults: true, oneofs: true, bytes: Buffer
})['zlink.framework.bench.withgrpc.BenchService'];
const bodyHex = '4b4e4c5a04030201011d00000008070605040302011817161514131211';
const wireHex = `0a1d${bodyHex}`;

test('raw request and response preserve the pre-protobuf 29-byte wire dump', () => {
  const body = Buffer.from(bodyHex, 'hex');
  assert.ok(header.isExpected(header.decode(body), 0x01020304, header.PHASE_ACTIVE,
    29, 0x0102030405060708n));
  const encoded = raw.encodeBenchPayloadMessage(body);
  try {
    assert.equal(encoded.data().toString('hex'), wireHex);
    assert.deepEqual(encoded.data(), service.Echo.requestSerialize(new BenchPayload(body)));
    const decoded = raw.decodeBenchPayloadBody(encoded.data());
    assert.deepEqual(decoded, body);
    const reply = raw.encodeBenchPayloadMessage(decoded);
    try {
      assert.equal(reply.data().toString('hex'), wireHex);
      assert.deepEqual(reply.data(), service.Echo.responseSerialize(new BenchPayload(body)));
    } finally {
      reply.close();
    }
  } finally {
    encoded.close();
  }
});

test('framework schema serializer preserves the same protobuf body bytes', () => {
  const body = Buffer.from(bodyHex, 'hex');
  const serializer = createBenchPayloadSerializer();
  const encoded = serializer.serialize(new BenchPayload(body));
  assert.equal(Buffer.from(encoded.data()).toString('hex'), wireHex);
  const decoded = serializer.deserialize(encoded);
  assert.deepEqual(decoded.body, body);
});

test('production L2 channel header preserves the raw protobuf body path', () => {
  const body = raw.encodeBenchPayloadMessage(Buffer.from(bodyHex, 'hex'));
  try {
    const header = raw.encodeProductionChannelHeader(1, 30000);
    const decoded = raw.decodeProductionChannelHeader([{ data: () => header }]);
    assert.equal(decoded.formatMarker, 0xf2);
    assert.equal(decoded.kind, 1);
    assert.equal(decoded.channelName, 'bench');
    assert.equal(decoded.messageName, 'BenchPayload');
    assert.equal(decoded.contentType, 'application/x-protobuf');
    assert.match(decoded.correlationId, /^[0-9a-f]{32}$/);
    assert.ok(decoded.deadline);
    assert.deepEqual(body.data(), Buffer.from(wireHex, 'hex'));
    const badHeader = Buffer.from(header.toString('utf8').replace('"formatMarker":242', '"formatMarker":0'));
    assert.throws(() => raw.decodeProductionChannelHeader([{ data: () => badHeader }]));
  } finally {
    body.close();
  }
});

test('production L2 reply envelope preserves protobuf body and request correlation', () => {
  const payload = Buffer.from(bodyHex, 'hex');
  const requestBytes = raw.encodeProductionChannelHeader(1, 30000);
  const requestHeader = raw.decodeProductionChannelHeader([{ data: () => requestBytes }]);
  const replyParts = raw.encodeProductionProtobufChannelReply(requestHeader, payload);
  const replyHeader = raw.decodeProductionChannelHeader([{ data: () => replyParts[0] }]);
  assert.equal(replyHeader.kind, 2);
  assert.equal(replyHeader.channelName, 'bench');
  assert.equal(replyHeader.messageName, 'BenchPayload');
  assert.equal(replyHeader.contentType, 'application/x-protobuf');
  assert.equal(replyHeader.correlationId, requestHeader.correlationId);
  assert.ok(Buffer.isBuffer(replyParts[1]));
  assert.deepEqual(raw.decodeBenchPayloadBody(replyParts[1]), payload);
});

for (const size of [127, 128, 1024, 4096]) {
  test(`raw protobuf wire remains identical for ${size}-byte body`, () => {
    const body = header.createPayloadBytes(size, 0x01020304, header.PHASE_ACTIVE,
      0x0102030405060708n);
    body.writeBigUInt64LE(0x1112131415161718n, 21);
    const encoded = raw.encodeBenchPayloadMessage(body);
    try {
      assert.deepEqual(encoded.data(), service.Echo.requestSerialize(new BenchPayload(body)));
      assert.deepEqual(raw.decodeBenchPayloadBody(encoded.data()), body);
    } finally {
      encoded.close();
    }
  });
}
