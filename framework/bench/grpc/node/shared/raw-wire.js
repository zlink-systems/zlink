// SPDX-License-Identifier: MPL-2.0
'use strict';

const path = require('node:path');
const protoLoader = require('@grpc/proto-loader');
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

// Fire-and-forget sends carry the command kind (3), like production command headers.
const COMMAND_ENVELOPE = Buffer.from(
  '{"kind":3,"channelName":"bench","messageName":"BenchPayload",'
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
let productionServiceWire;

function requireProductionChannelEnvelope() {
  if (productionChannelEnvelope !== undefined) return productionChannelEnvelope;
  const root = path.resolve(__dirname, '../../../../languages/node/packages/framework/dist');
  const {
    encodeChannelEnvelopeParts,
    encodeChannelReplyParts,
    decodeChannelHeader,
    decodeChannelPayload,
    decodeChannelReply
  } = require(path.join(root, 'runtime/channels/channel-envelope.js'));
  const { createBenchPayloadSerializer } = require('./framework-protobuf');
  const codecs = {
    serializers: new Map([['application/x-protobuf', createBenchPayloadSerializer()]])
  };
  const typedCodecs = { serializers: new Map() };
  productionChannelEnvelope = {
    encodeChannelEnvelopeParts,
    encodeChannelReplyParts,
    decodeChannelHeader,
    decodeChannelPayload,
    decodeChannelReply,
    codecs,
    typedCodecs
  };
  return productionChannelEnvelope;
}

function encodeProductionProtobufChannelPayload(kind, payload, timeoutMs, correlationId) {
  const { encodeChannelEnvelopeParts, codecs } = requireProductionChannelEnvelope();
  return encodeChannelEnvelopeParts(
    kind,
    'bench',
    'BenchPayload',
    new BenchPayload(payload),
    timeoutMs,
    undefined,
    codecs,
    correlationId,
    false
  );
}

function decodeProductionChannelHeader(parts) {
  return requireProductionChannelEnvelope().decodeChannelHeader(parts, false);
}

function encodeProductionProtobufChannelReply(requestHeader, payload) {
  const { encodeChannelReplyParts, codecs } = requireProductionChannelEnvelope();
  return encodeChannelReplyParts(requestHeader, new BenchPayload(payload), codecs);
}

function decodeProductionProtobufReply(parts) {
  const { decodeChannelReply, codecs } = requireProductionChannelEnvelope();
  return decodeChannelReply(parts, codecs).body;
}

function encodeProductionTypedChannelPayload(kind, payload, timeoutMs, correlationId) {
  const { encodeChannelEnvelopeParts, typedCodecs } = requireProductionChannelEnvelope();
  return encodeChannelEnvelopeParts(
    kind,
    'bench',
    'BenchPayload',
    payload,
    timeoutMs,
    undefined,
    typedCodecs,
    correlationId,
    false
  );
}

function encodeProductionTypedChannelReply(requestHeader, payload) {
  const { encodeChannelReplyParts, typedCodecs } = requireProductionChannelEnvelope();
  return encodeChannelReplyParts(requestHeader, payload, typedCodecs);
}

function decodeProductionTypedPayload(encoded, header) {
  const { decodeChannelPayload, typedCodecs } = requireProductionChannelEnvelope();
  return decodeChannelPayload({
    header,
    packetName: 'BenchPayload',
    payload: encoded
  }, typedCodecs);
}

function decodeProductionTypedReply(parts) {
  const { decodeChannelReply, typedCodecs } = requireProductionChannelEnvelope();
  const payload = decodeChannelReply(parts, typedCodecs);
  return Buffer.from(payload.body, 'base64');
}

function requireProductionServiceWire() {
  if (productionServiceWire !== undefined) return productionServiceWire;
  const root = path.resolve(__dirname, '../../../../languages/node/packages/framework/dist');
  const serviceWire = require(path.join(root, 'runtime/foundation/service-wire-m6a-codec.js'));
  const constants = require(path.join(root, 'runtime/foundation/service-wire-constants.generated.js'));
  const { ZLinkBufferMessage } = require(path.join(root, 'runtime/backend/runtime-message.js'));
  productionServiceWire = { ...serviceWire, ...constants, ZLinkBufferMessage };
  return productionServiceWire;
}

function encodeProductionServiceRequest(request, correlation, parts) {
  const wire = requireProductionServiceWire();
  const header = request
    ? wire.encodeChannelRequestHeader(correlation, 'bench')
    : wire.encodeChannelSendHeader('bench');
  const frame = wire.encodeFrameworkMultipartApplicationMessage(parts);
  return { header, frame };
}

function encodeProductionServiceReply(correlation, parts) {
  const wire = requireProductionServiceWire();
  return {
    header: wire.encodeReplyHeader(correlation),
    frame: wire.encodeFrameworkMultipartApplicationMessage(parts)
  };
}

function decodeProductionServiceRequest(parts, request) {
  const wire = requireProductionServiceWire();
  if (parts.length !== 2) throw new Error('invalid service-wire application part count');
  const headerBytes = parts[0].data();
  const header = wire.decodeHeader(headerBytes);
  let correlation = null;
  if (request) {
    const decoded = wire.decodeChannelRequestHeader(headerBytes, header);
    if (decoded.channelName !== 'bench') throw new Error('invalid service-wire request channel');
    correlation = decoded.correlation;
  } else if (wire.decodeChannelSendHeader(headerBytes, header) !== 'bench') {
    throw new Error('invalid service-wire command channel');
  }
  const frame = wire.decodeMultipartApplicationPayloadView(parts[1].data());
  const messageParts = wire.decodeMultipartParts(
    frame.payload,
    wire.ZLinkBufferMessage.fromOwned,
    (part) => part.close()
  );
  if (messageParts.length !== 2) {
    for (const part of messageParts) part.close();
    throw new Error('invalid channel request part count');
  }
  return { correlation, parts: messageParts };
}

function decodeProductionServiceReply(parts, correlation) {
  const wire = requireProductionServiceWire();
  if (parts.length !== 2) throw new Error('invalid service-wire reply part count');
  const header = wire.decodeReplyHeader(parts[0].data());
  if (header.correlation !== correlation || header.terminalResult !== 0 || header.failureCode !== 0) {
    throw new Error('invalid service-wire reply header');
  }
  const frame = wire.decodeMultipartApplicationPayloadView(parts[1].data());
  const messageParts = wire.decodeMultipartParts(
    frame.payload,
    wire.ZLinkBufferMessage.fromOwned,
    (part) => part.close()
  );
  if (messageParts.length !== 2) {
    for (const part of messageParts) part.close();
    throw new Error('invalid channel reply part count');
  }
  return messageParts;
}

// Reuse the same protobuf serializer as the gRPC row; schema loading is setup work.
const payloadMethod = protoLoader.loadSync(path.join(__dirname, '../../proto/bench.proto'), {
  keepCase: true, longs: String, enums: String, defaults: true, oneofs: true, bytes: Buffer
})['zlink.framework.bench.withgrpc.BenchService'].Echo;

/**
 * Construct and serialize a typed message for every request and response. The encoded
 * bytes go to the binding as they are: wrapping them in zlink.Message.from first adds a
 * copy that the best binding usage (and the Framework payload path) does not make.
 */
function encodeBenchPayloadBytes(payload) {
  return payloadMethod.requestSerialize(new BenchPayload(payload));
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
  COMMAND_ENVELOPE,
  RESPONSE_ENVELOPE,
  frameworkEnvelope,
  requireProductionChannelEnvelope,
  encodeProductionProtobufChannelPayload,
  decodeProductionProtobufReply,
  decodeProductionChannelHeader,
  encodeProductionProtobufChannelReply,
  encodeProductionTypedChannelPayload,
  encodeProductionTypedChannelReply,
  decodeProductionTypedPayload,
  decodeProductionTypedReply,
  requireProductionServiceWire,
  encodeProductionServiceRequest,
  encodeProductionServiceReply,
  decodeProductionServiceRequest,
  decodeProductionServiceReply,
  ROUTING_IDS,
  encodeBenchPayloadBytes,
  decodeBenchPayloadBody
};
