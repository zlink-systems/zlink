// SPDX-License-Identifier: MPL-2.0
'use strict';

const path = require('node:path');
const protoLoader = require('@grpc/proto-loader');
const zlink = require('@zlink-systems/zlink');
const { BenchPayload } = require('./framework-bench-contract');

// Wire shape of the `zlink-<lang>` raw row.
//
// The raw row is measured against `zlink-c` (spec section 7.2 formula 1), so it must put
// the same bytes on the wire as `framework/bench/grpc/c`. That bench sends a
// two-part message: an envelope header part and a protobuf-encoded BenchPayload
// part (bench_zlink_client.cpp:15 and :126-140). The .NET raw row does the same
// (ZLinkRawServer/Program.cs RawEnvelopeHeaders). Sending a bare payload here
// would make formula 1 divide two different experiments.

const REQUEST_ENVELOPE = Buffer.from(
  '{"kind":1,"channelName":"bench","messageName":"BenchPayload",'
  + '"contentType":"application/x-protobuf","correlationId":null,"deadline":null,'
  + '"topic":null,"errorCode":null,"errorMessage":null,"source":null}',
  'utf8'
);

const RESPONSE_ENVELOPE = Buffer.from(
  '{"kind":2,"channelName":"bench","messageName":"BenchPayload",'
  + '"contentType":"application/x-protobuf","correlationId":null,"deadline":null,'
  + '"topic":null,"errorCode":null,"errorMessage":null,"source":null}',
  'utf8'
);

function frameworkEnvelope(kind) {
  const correlationId = kind === 1
    ? require('node:crypto').randomUUID().replaceAll('-', '')
    : null;
  const header = Buffer.from(JSON.stringify({
    formatMarker: 242,
    kind,
    channelName: 'bench',
    messageName: 'BenchPayload',
    contentType: 'application/x-protobuf',
    correlationId,
    deadline: null,
    topic: null,
    metadata: {}
  }), 'utf8');
  return Buffer.concat([Buffer.from([0xf2]), header]);
}

let productionChannelEnvelope;

function requireProductionChannelEnvelope() {
  if (productionChannelEnvelope !== undefined) return productionChannelEnvelope;
  const root = path.resolve(__dirname, '../../../../languages/node/packages/framework/dist');
  const {
    encodeChannelEnvelopeParts,
    decodeChannelHeader
  } = require(path.join(root, 'runtime/channels/channel-envelope.js'));
  const { ZLinkEncodedPayload } = require(path.join(root, 'contracts/Common/ZLinkEncodedPayload.js'));
  const body = ZLinkEncodedPayload.fromOwned(Buffer.alloc(0));
  const codecs = {
    serializers: new Map([['application/x-protobuf', {
      serialize() { return body; },
      deserialize() { throw new Error('benchmark header codec is encode-only'); }
    }]])
  };
  productionChannelEnvelope = { encodeChannelEnvelopeParts, decodeChannelHeader, codecs };
  return productionChannelEnvelope;
}

function encodeProductionChannelHeader(kind, timeoutMs) {
  const { encodeChannelEnvelopeParts, codecs } = requireProductionChannelEnvelope();
  const parts = encodeChannelEnvelopeParts(
    kind,
    'bench',
    'BenchPayload',
    {},
    timeoutMs,
    undefined,
    codecs,
    undefined,
    false
  );
  return parts[0];
}

function decodeProductionChannelHeader(parts) {
  return requireProductionChannelEnvelope().decodeChannelHeader(parts, false);
}

// Reuse the same protobuf serializer as the gRPC row; schema loading is setup work.
const payloadMethod = protoLoader.loadSync(path.join(__dirname, '../../proto/bench.proto'), {
  keepCase: true, longs: String, enums: String, defaults: true, oneofs: true, bytes: Buffer
})['zlink.framework.bench.withgrpc.BenchService'].Echo;

/** Construct and serialize a typed message for every request and response. */
function encodeBenchPayloadMessage(payload) {
  return zlink.Message.from(payloadMethod.requestSerialize(new BenchPayload(payload)));
}

/** Protobuf decode errors propagate to the caller's existing error accounting. */
function decodeBenchPayloadBody(encoded) {
  return payloadMethod.requestDeserialize(encoded).body;
}

const ROUTING_IDS = {
  rawRequestServer: 'bench-raw-request-server',
  rawCommandServer: 'bench-raw-command-server'
};

module.exports = {
  REQUEST_ENVELOPE,
  RESPONSE_ENVELOPE,
  frameworkEnvelope,
  requireProductionChannelEnvelope,
  encodeProductionChannelHeader,
  decodeProductionChannelHeader,
  ROUTING_IDS,
  encodeBenchPayloadMessage,
  decodeBenchPayloadBody
};
