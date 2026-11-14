// SPDX-License-Identifier: MPL-2.0
'use strict';

const path = require('node:path');
const protoLoader = require('@grpc/proto-loader');
const { ZLinkEncodedPayload } = require('@zlink-systems/framework');

const definition = protoLoader.loadSync(path.join(__dirname, '../../proto/bench.proto'), {
  keepCase: true, longs: String, enums: String, defaults: true, oneofs: true, bytes: Buffer
});
const payloadMethod = definition['zlink.framework.bench.withgrpc.BenchService'].Echo;

function createBenchPayloadSerializer() {
  return {
    serialize(value) {
      return ZLinkEncodedPayload.fromOwned(payloadMethod.requestSerialize(value));
    },
    deserialize(payload) {
      return payloadMethod.requestDeserialize(payload.view());
    }
  };
}

module.exports = { createBenchPayloadSerializer };
