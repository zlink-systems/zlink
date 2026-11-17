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
  ROUTING_IDS,
  encodeBenchPayloadMessage,
  decodeBenchPayloadBody
};
