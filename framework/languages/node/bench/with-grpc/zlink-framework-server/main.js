// SPDX-License-Identifier: MPL-2.0
'use strict';

// `zlink-framework-<lang>` server, node row.
//
// spec section 1.3: RouteMesh ROUTER<->ROUTER with a channel request handler and a
// channel send handler. The host is `@zlink-systems/nestjs`, which is the public
// way to stand up `packages/framework` -- the analogue of `.NET`'s
// `Zlink.Framework.AspNetCore`. `packages/framework`'s `src/internal.ts` is
// marked "not exported by package.json" and is not used: reaching into it would
// breach G4 (public API only).

require('reflect-metadata');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { ZLinkModule, zlinkFramework } = require('@zlink-systems/nestjs');
const {
  createProtobufMessageSerializer,
  ZLINK_PROTOBUF_CONTENT_TYPE
} = require('@zlink-systems/framework-codec-protobuf/framework');

const { argValue } = require('../shared/args');
const { BenchServerMetrics, startStatsServer } = require('../shared/bench-server-metrics');
const contract = require('../shared/framework-bench-contract');

const argv = process.argv.slice(2);
const endpoint = argValue(argv, '--endpoint', 'tcp://127.0.0.1:5082');
const metricsUrl = argValue(argv, '--metrics-url', 'http://127.0.0.1:5083');

const metrics = new BenchServerMetrics();

class EchoHandler {
  // spec section 2: `request-serial` and `request-window` echo the payload back so the
  // client can validate the 29-byte header it sent (G2).
  async handle(request) {
    return request;
  }
}
Injectable()(EchoHandler);

class CommandHandler {
  // spec section 5 / G3: the send row's throughput is this count, taken on the server.
  async handle(message) {
    metrics.record(toBuffer(message && message.body));
    return undefined;
  }
}
Injectable()(CommandHandler);

function toBuffer(body) {
  if (body === undefined || body === null) return Buffer.alloc(0);
  if (Buffer.isBuffer(body)) return body;
  if (body instanceof Uint8Array) return Buffer.from(body.buffer, body.byteOffset, body.byteLength);
  return Buffer.from(body);
}

class BenchServerModule {}
Module({
  imports: [
    ZLinkModule.forRootFactory({
      inject: [],
      useFactory: () => {
        const builder = zlinkFramework();
        builder.codecs().use({
          register: (codecs) => {
            codecs.addSerializer(ZLINK_PROTOBUF_CONTENT_TYPE, createProtobufMessageSerializer());
          }
        });
        const mesh = builder.addRouteMesh(contract.MESH_NAME)
          .listen(endpoint)
          .routingId(contract.SERVER_ROUTING_ID);
        mesh.channel(contract.CHANNEL_NAME).server()
          .addRequestHandler(contract.PACKET_NAME, EchoHandler)
          .addSendHandler(contract.PACKET_NAME, CommandHandler);
        return builder.build();
      }
    })
  ],
  providers: [EchoHandler, CommandHandler]
})(BenchServerModule);

(async () => {
  const app = await NestFactory.createApplicationContext(BenchServerModule, {
    logger: false,
    abortOnError: false
  });
  startStatsServer(metricsUrl, metrics, {
    implementation: 'zlink-framework-node',
    host: '@zlink-systems/nestjs',
    codec: 'framework-codec-protobuf',
    endpoint
  });
  process.stderr.write(`[framework-server] endpoint=${endpoint} stats=${metricsUrl}\n`);
  process.on('SIGTERM', async () => { await app.close(); process.exit(0); });
})().catch((error) => {
  console.error(`framework server failed: ${error && error.stack}`);
  process.exit(1);
});
