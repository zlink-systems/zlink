// SPDX-License-Identifier: MPL-2.0
'use strict';

require('reflect-metadata');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { ZLinkModule, ZLINK_ROUTE_CLIENT, zlinkFramework } = require('@zlink-systems/nestjs');
const {
  ZLINK_PROTOBUF_CONTENT_TYPE
} = require('@zlink-systems/framework-codec-protobuf/framework');

const contract = require('../shared/framework-bench-contract');
const { createBenchPayloadSerializer } = require('../shared/framework-protobuf');
const gamePayload = require('../shared/game-payload');
const header = require('../shared/bench-metric-header');

async function createFrameworkTransport(options) {
  const jsonBody = options.implementation === 'zlink-framework-node-json';
  const packetName = jsonBody ? 'BenchGameMessage' : contract.PACKET_NAME;
  const createPayload = jsonBody
    ? (size, run, phase, sequence) =>
        gamePayload.createGameMessage(
          size,
          run,
          phase,
          sequence,
          header.nowNs(),
          options.implementation
        )
    : undefined;
  class BenchClientModule {}
  Module({
    imports: [
      ZLinkModule.forRootFactory({
        inject: [],
        useFactory: () => {
          const builder = zlinkFramework();
          if (!jsonBody) {
            builder.codecs().use({
              register: (codecs) => {
                codecs.addSerializer(ZLINK_PROTOBUF_CONTENT_TYPE, createBenchPayloadSerializer());
              }
            });
          }
          const mesh = builder
            .addRouteMesh(contract.MESH_NAME)
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
  const requestSubmission = jsonBody
    ? (_stream, message) => {
        const request = client
          .requestToNode(contract.MESH_NAME, contract.SERVER_ROUTING_ID, message)
          .timeout(options.requestTimeoutMs)
          .submit();
        return { admitted: request.admitted, reply: request.then((reply) => reply) };
      }
    : (_stream, body) => {
        const request = client
          .requestToNode(
            contract.MESH_NAME,
            contract.SERVER_ROUTING_ID,
            new contract.BenchPayload(body)
          )
          .timeout(options.requestTimeoutMs)
          .submit();
        return { admitted: request.admitted, reply: request.then((reply) => toBuffer(reply.body)) };
      };
  const send = jsonBody
    ? async (_stream, message) => {
        await client.sendToNode(contract.MESH_NAME, contract.SERVER_ROUTING_ID, message).submit();
      }
    : async (_stream, body) => {
        await client
          .sendToNode(
            contract.MESH_NAME,
            contract.SERVER_ROUTING_ID,
            new contract.BenchPayload(body)
          )
          .submit();
      };
  return {
    createPayload,
    requestSubmission,
    request: (_stream, body) => requestSubmission(_stream, body).reply,
    send,
    validateReply: jsonBody
      ? (reply, run, phase, size, sequence) =>
          gamePayload.assertGameMessageStamp(
            reply,
            run,
            phase,
            sequence,
            size,
            options.implementation
          )
      : undefined,
    close: async () => app.close()
  };
}

function toBuffer(value) {
  if (Buffer.isBuffer(value)) return value;
  return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
}

module.exports = { createFrameworkTransport };
