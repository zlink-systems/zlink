// SPDX-License-Identifier: MPL-2.0
'use strict';

const path = require('node:path');
const protoLoader = require('@grpc/proto-loader');
const {
  createProtobufEnvelopeMessageSerializer
} = require('@zlink-systems/framework-codec-protobuf/framework');

const definition = protoLoader.loadSync(path.join(__dirname, '../../proto/bench.proto'), {
  keepCase: true, longs: String, enums: String, defaults: true, oneofs: true, bytes: Buffer
});
const payloadMethod = definition['zlink.framework.bench.withgrpc.BenchService'].Echo;

function createBenchPayloadSerializer() {
  return createProtobufEnvelopeMessageSerializer({
    encode(value) {
      return { payload: payloadMethod.requestSerialize(value) };
    },
    decode(payload) {
      return payloadMethod.requestDeserialize(toBuffer(payload.payload));
    }
  });
}

function toBuffer(value) {
  if (Buffer.isBuffer(value)) return value;
  return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
}

module.exports = { createBenchPayloadSerializer };
