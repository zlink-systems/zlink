// SPDX-License-Identifier: MPL-2.0
'use strict';

// Wire shape of the `zlink-<lang>` raw row.
//
// The raw row is measured against `zlink-c` (spec section 7.2 formula 1), so it must put
// the same bytes on the wire as `bindings/c/bench/with_grpc`. That bench sends a
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

function varintSize(value) {
  let size = 1;
  let remaining = value >>> 0;
  while (remaining >= 0x80) {
    remaining >>>= 7;
    size += 1;
  }
  return size;
}

function writeVarint(buffer, offset, value) {
  let remaining = value >>> 0;
  let index = offset;
  while (remaining >= 0x80) {
    buffer[index++] = (remaining & 0x7f) | 0x80;
    remaining >>>= 7;
  }
  buffer[index++] = remaining;
  return index;
}

/** `BenchPayload { bytes body = 1 }` around an already-stamped payload. */
function encodeBenchPayload(payload) {
  const encoded = Buffer.allocUnsafe(1 + varintSize(payload.length) + payload.length);
  encoded[0] = 0x0a;
  const offset = writeVarint(encoded, 1, payload.length);
  payload.copy(encoded, offset);
  return encoded;
}

/** Returns a subarray view of field 1, or null. No copy. */
function decodeBenchPayloadBody(encoded) {
  let offset = 0;
  while (offset < encoded.length) {
    let key = 0;
    let shift = 0;
    let byte;
    do {
      if (offset >= encoded.length || shift >= 32) return null;
      byte = encoded[offset++];
      key |= (byte & 0x7f) << shift;
      shift += 7;
    } while (byte & 0x80);
    const field = key >>> 3;
    const wireType = key & 0x07;
    if (wireType !== 2) return null;
    let length = 0;
    shift = 0;
    do {
      if (offset >= encoded.length || shift >= 32) return null;
      byte = encoded[offset++];
      length |= (byte & 0x7f) << shift;
      shift += 7;
    } while (byte & 0x80);
    if (encoded.length - offset < length) return null;
    if (field === 1) return encoded.subarray(offset, offset + length);
    offset += length;
  }
  return null;
}

const ROUTING_IDS = {
  rawRequestServer: 'bench-raw-request-server',
  rawCommandServer: 'bench-raw-command-server'
};

module.exports = {
  REQUEST_ENVELOPE,
  RESPONSE_ENVELOPE,
  ROUTING_IDS,
  encodeBenchPayload,
  decodeBenchPayloadBody
};
