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
  const { ZLinkEncodedPayload } = require(path.join(root, 'contracts/Common/ZLinkEncodedPayload.js'));
  const body = ZLinkEncodedPayload.fromOwned(Buffer.alloc(0));
  const codecs = {
    serializers: new Map([['application/x-protobuf', {
      serialize() { return body; },
      deserialize() { throw new Error('benchmark header codec is encode-only'); }
    }]])
  };
  const replyCodecs = {
    serializers: new Map([['application/x-protobuf', {
      serialize(value) {
        return ZLinkEncodedPayload.fromOwned(
          payloadMethod.requestSerialize(new BenchPayload(value.body))
        );
      },
      deserialize() { throw new Error('benchmark reply codec is encode-only'); }
    }]])
  };
  productionChannelEnvelope = {
    encodeChannelEnvelopeParts,
    encodeChannelReplyParts,
    decodeChannelHeader,
    decodeChannelPayload,
    decodeChannelReply,
    codecs,
    replyCodecs,
    ZLinkEncodedPayload
  };
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

function encodeProductionProtobufChannelReply(requestHeader, payload) {
  const { encodeChannelReplyParts, replyCodecs } = requireProductionChannelEnvelope();
  return encodeChannelReplyParts(requestHeader, { body: payload }, replyCodecs);
}

function encodeProductionTypedChannelPayload(kind, payload, timeoutMs, correlationId) {
  const { encodeChannelEnvelopeParts } = requireProductionChannelEnvelope();
  const parts = encodeChannelEnvelopeParts(
    kind,
    'bench',
    'BenchPayload',
    payload,
    timeoutMs,
    undefined,
    undefined,
    correlationId,
    false
  );
  return [parts[0], zlink.Message.from(Buffer.from(parts[1], 'utf8'))];
}

function encodeProductionTypedChannelReply(requestHeader, payload) {
  const { encodeChannelReplyParts } = requireProductionChannelEnvelope();
  const parts = encodeChannelReplyParts(requestHeader, payload);
  return [parts[0], zlink.Message.from(Buffer.from(parts[1], 'utf8'))];
}

function decodeProductionTypedPayload(encoded, header) {
  const { decodeChannelPayload } = requireProductionChannelEnvelope();
  const payload = decodeChannelPayload({
    header,
    packetName: 'BenchPayload',
    payload: Buffer.from(encoded)
  });
  return Buffer.from(payload.body, 'base64');
}

function decodeProductionTypedReply(parts) {
  const { decodeChannelReply } = requireProductionChannelEnvelope();
  const payload = decodeChannelReply(parts);
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
  const frame = wire.encodeMultipartApplicationPayload(
    parts.map((part) => Buffer.isBuffer(part) ? part : part.data()),
    wire.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
    wire.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
  );
  return { header, frame };
}

function encodeProductionServiceReply(correlation, parts) {
  const wire = requireProductionServiceWire();
  return {
    header: wire.encodeReplyHeader(correlation),
    frame: wire.encodeMultipartApplicationPayload(
      parts.map((part) => Buffer.isBuffer(part) ? part : part.data()),
      wire.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME,
      wire.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
    )
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
  encodeBenchPayloadMessage,
  decodeBenchPayloadBody
};
