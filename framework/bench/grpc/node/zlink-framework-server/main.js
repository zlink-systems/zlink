// SPDX-License-Identifier: MPL-2.0
'use strict';

// `zlink-framework-<lang>` server, node row.
//
// spec section 1.3: RouteMesh ROUTER<->ROUTER with node-direct request and send
// handlers. The host is `@zlink-systems/nestjs`, which is the public
// way to stand up `packages/framework` -- the analogue of `.NET`'s
// `Zlink.Framework.AspNetCore`. `packages/framework`'s `src/internal.ts` is
// marked "not exported by package.json" and is not used: reaching into it would
// breach G4 (public API only).

require('reflect-metadata');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { ZLinkModule, zlinkFramework } = require('@zlink-systems/nestjs');
const {
  ZLINK_PROTOBUF_CONTENT_TYPE
} = require('@zlink-systems/framework-codec-protobuf/framework');

const { argValue, argInt } = require('../shared/args');
const { BenchServerMetrics, startStatsServer } = require('../shared/bench-server-metrics');
const { RESPONSE_ENVELOPE, decodeBenchPayloadBody, encodeBenchPayloadMessage } = require('../shared/raw-wire');
const contract = require('../shared/framework-bench-contract');
const { createBenchPayloadSerializer } = require('../shared/framework-protobuf');
const zlink = require('@zlink-systems/zlink');
const { EchoHandler, CommandHandler } = require('./bench-handlers');

const argv = process.argv.slice(2);
const endpoint = argValue(argv, '--endpoint', 'tcp://127.0.0.1:5234');
const metricsUrl = argValue(argv, '--metrics-url', 'http://127.0.0.1:5235');
const batch = argInt(argv, '--recv-batch', 256);

const metrics = new BenchServerMetrics();
Injectable()(EchoHandler);
Injectable()(CommandHandler);

if (process.env.ZLINK_LADDER_L0_SERVER === '1') {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  router.setRoutingId(zlink.RoutingId.from(Buffer.from('bench-server', 'ascii')));
  router.options.mandatory = true;
  router.bind(endpoint);
  startStatsServer(metricsUrl, metrics, { implementation: 'zlink-framework-node-l0', endpoint });
  const received = new zlink.Received();
  const frameworkWire = process.env.ZLINK_LADDER_L0_FRAMEWORK_CLIENT === '1'
    || process.env.ZLINK_CROSS_FRAMEWORK_WIRE === '1';
  const echoHandler = new EchoHandler(metrics);
  const commandHandler = new CommandHandler(metrics);

  function frameworkResponseHeader(bytes) {
    const header = JSON.parse(bytes.subarray(1).toString('utf8'));
    header.kind = 2;
    return Buffer.concat([Buffer.from([0xf2]), Buffer.from(JSON.stringify(header), 'utf8')]);
  }

  function pump() {
    let handled = 0;
    while (handled < batch) {
      let hasMessage;
      try {
        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
      } catch (error) {
        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
        metrics.recordError();
        console.error(`ladder L0 recv failed: ${error.message}`);
        break;
      }
      if (!hasMessage) break;
      handled += 1;
      const parts = received.parts;
      try {
        const header = frameworkWire ? parts[0].data() : null;
        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
        if (payload === null) throw new Error('invalid benchmark payload');
        if (received.replyToken !== null) {
          echoHandler.handleL0(payload);
          received.reply()
            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
            .message(encodeBenchPayloadMessage(payload))
            .submit();
        } else {
          commandHandler.handleL0(payload);
        }
      } catch (error) {
        metrics.recordError();
        console.error(`ladder L0 handler failed: ${error.message}`);
      }
    }
    setImmediate(pump);
  }

  process.stderr.write(`[framework-server-l0] endpoint=${endpoint} stats=${metricsUrl}\n`);
  process.on('SIGTERM', () => {
    router.close();
    ctx.close();
    process.exit(0);
  });
  setImmediate(pump);
} else {

class BenchServerModule {}
Module({
  imports: [
    ZLinkModule.forRootFactory({
      inject: [],
      useFactory: () => {
        const builder = zlinkFramework();
        builder.codecs().use({
          register: (codecs) => {
            codecs.addSerializer(ZLINK_PROTOBUF_CONTENT_TYPE, createBenchPayloadSerializer());
          }
        });
        const mesh = builder.addRouteMesh(contract.MESH_NAME)
          .listen(endpoint)
          .routingId(contract.SERVER_ROUTING_ID);
        mesh.addRequestHandler(contract.PACKET_NAME, EchoHandler);
        mesh.addSendHandler(contract.PACKET_NAME, CommandHandler);
        return builder.build();
      }
    })
  ],
  providers: [
    { provide: EchoHandler, useFactory: () => new EchoHandler(metrics) },
    { provide: CommandHandler, useFactory: () => new CommandHandler(metrics) }
  ]
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
}
