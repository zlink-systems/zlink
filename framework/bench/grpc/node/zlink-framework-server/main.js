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

const { argValue } = require('../shared/args');
const { BenchServerMetrics, startStatsServer } = require('../shared/bench-server-metrics');
const contract = require('../shared/framework-bench-contract');
const { createBenchPayloadSerializer } = require('../shared/framework-protobuf');
const { Worker } = require('node:worker_threads');
const { EchoHandler, CommandHandler } = require('./bench-handlers');

const argv = process.argv.slice(2);
const endpoint = argValue(argv, '--endpoint', 'tcp://127.0.0.1:5234');
const metricsUrl = argValue(argv, '--metrics-url', 'http://127.0.0.1:5235');

const metrics = new BenchServerMetrics();
Injectable()(EchoHandler);
Injectable()(CommandHandler);

if (process.env.ZLINK_LADDER_L0_SERVER === '1') {
  const shared = new SharedArrayBuffer(16 + 500000 * Float64Array.BYTES_PER_ELEMENT);
  const counters = new Int32Array(shared, 0, 3);
  const latencyCount = new Int32Array(shared, 12, 1);
  const latencies = new Float64Array(shared, 16, 500000);
  const snapshot = metrics.snapshot.bind(metrics);
  const reset = metrics.reset.bind(metrics);
  metrics.snapshot = () => {
    const current = snapshot();
    const count = Math.min(Atomics.load(latencyCount, 0), latencies.length);
    const samples = Array.from(latencies.subarray(0, count)).sort((a, b) => a - b);
    return {
      ...current,
      completed: Atomics.load(counters, 0),
      received: Atomics.load(counters, 0),
      activeMessages: Atomics.load(counters, 0),
      allPhaseMessages: Atomics.load(counters, 1),
      errors: Atomics.load(counters, 2),
      meanMicros: samples.length ? samples.reduce((sum, value) => sum + value, 0) / samples.length / 1000 : 0,
      p50Micros: samples[Math.floor(samples.length * 0.5)] / 1000 || 0,
      p95Micros: samples[Math.floor(samples.length * 0.95)] / 1000 || 0,
      p99Micros: samples[Math.floor(samples.length * 0.99)] / 1000 || 0
    };
  };
  metrics.reset = () => {
    reset();
    Atomics.store(counters, 0, 0);
    Atomics.store(counters, 1, 0);
    Atomics.store(counters, 2, 0);
    Atomics.store(latencyCount, 0, 0);
  };
  const worker = new Worker(require.resolve('./l0-worker'), {
    workerData: { endpoint, shared }
  });
  let statsServerStarted = false;
  worker.on('message', (message) => {
    if (message.ready && !statsServerStarted) {
      statsServerStarted = true;
      startStatsServer(metricsUrl, metrics, { implementation: 'zlink-framework-node-l0', endpoint });
      process.stderr.write(`[framework-server-l0] endpoint=${endpoint} stats=${metricsUrl}\n`);
      return;
    }
    if (message.error) {
      if (!message.counted) Atomics.add(counters, 2, 1);
      console.error(`[ladder-l0] ${message.error}`);
    }
  });
  worker.on('error', (error) => console.error(`[ladder-l0] worker failed: ${error.stack}`));
  process.on('SIGTERM', async () => { await worker.terminate(); process.exit(0); });
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
