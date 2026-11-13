// SPDX-License-Identifier: MPL-2.0
'use strict';

require('reflect-metadata');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const {
  ZLinkModule,
  ZLINK_ROUTE_CLIENT,
  zlinkFramework
} = require('@zlink-systems/nestjs');
const contract = require('../shared/framework-bench-contract');
const ladderL5ToL7 = process.env.ZLINK_LADDER_L5_7_CLIENT === '1';
let ZLINK_PROTOBUF_CONTENT_TYPE;
let createBenchPayloadSerializer;
if (!ladderL5ToL7) {
  ({ ZLINK_PROTOBUF_CONTENT_TYPE } = require(
    '@zlink-systems/framework-codec-protobuf/framework'
  ));
  ({ createBenchPayloadSerializer } = require('../shared/framework-protobuf'));
}

async function createFrameworkTransport(options) {
  class BenchClientModule {}
  Module({
    imports: [
      ZLinkModule.forRootFactory({
        inject: [],
        useFactory: () => {
          const builder = zlinkFramework();
          if (!ladderL5ToL7) {
            builder.codecs().use({
              register: (codecs) => {
                codecs.addSerializer(
                  ZLINK_PROTOBUF_CONTENT_TYPE,
                  createBenchPayloadSerializer()
                );
              }
            });
          }
          const mesh = builder.addRouteMesh(contract.MESH_NAME)
            .listen('tcp://127.0.0.1:0')
            .routingId(`${contract.CLIENT_ROUTING_ID}-${process.pid}`);
          mesh.peerConnections().connect(contract.SERVER_ROUTING_ID, options.targetEndpoint);
          return builder.build();
        }
      })
    ]
  })(BenchClientModule);

  const app = await NestFactory.createApplicationContext(BenchClientModule, {
    logger: false,
    abortOnError: false
  });
  const client = app.get(ZLINK_ROUTE_CLIENT, { strict: false });
  const requestSubmission = (_stream, body) => {
    const request = client.requestToNode(
      contract.MESH_NAME,
      contract.SERVER_ROUTING_ID,
      new contract.BenchPayload(body)
    ).timeout(options.requestTimeoutMs).submit();
    return { admitted: request.admitted, reply: request.then(reply => toBuffer(reply.body)) };
  };
  return {
    requestSubmission,
    request: (_stream, body) => requestSubmission(_stream, body).reply,
    send: async (_stream, body) => {
      await client.sendToNode(
        contract.MESH_NAME,
        contract.SERVER_ROUTING_ID,
        new contract.BenchPayload(body)
      ).submit();
    },
    close: async () => app.close()
  };
}

function toBuffer(value) {
  if (Buffer.isBuffer(value)) return value;
  if (typeof value === 'string') return Buffer.from(value, 'base64');
  return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
}

module.exports = { createFrameworkTransport };
