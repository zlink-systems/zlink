// SPDX-License-Identifier: MPL-2.0
'use strict';

// `grpc-<lang>` server, node row. spec section 8.1: @grpc/grpc-js with
// @grpc/proto-loader, in the library's default server configuration -- spec
// section 8.2 fixes that gRPC is left at each language's default and the
// configuration is recorded rather than tuned.

const path = require('node:path');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const { argValue } = require('../shared/args');
const { BenchServerMetrics, startStatsServer } = require('../shared/bench-server-metrics');

const argv = process.argv.slice(2);
const url = argValue(argv, '--url', '127.0.0.1:5081');
const metricsUrl = argValue(argv, '--metrics-url', 'http://127.0.0.1:5084');

const definition = protoLoader.loadSync(path.join(__dirname, '..', 'proto', 'bench.proto'), {
  keepCase: true,
  longs: String,
  enums: String,
  defaults: true,
  oneofs: true,
  bytes: Buffer
});
const proto = grpc.loadPackageDefinition(definition).zlink.framework.bench.withgrpc;

const metrics = new BenchServerMetrics();
const server = new grpc.Server();

server.addService(proto.BenchService.service, {
  // spec section 2: `Echo` returns the payload, so the client can validate the
  // 29-byte header that came back (G2).
  Echo(call, callback) {
    callback(null, { body: call.request.body });
  },
  // FB-002: the send comparison keeps this unary Command returning Empty. No
  // client-streaming RPC is added and no RPC is added to the proto.
  Command(call, callback) {
    metrics.record(call.request.body);
    callback(null, {});
  }
});

server.bindAsync(url, grpc.ServerCredentials.createInsecure(), (error, port) => {
  if (error) {
    console.error(`grpc server bind failed: ${error.message}`);
    process.exit(1);
  }
  startStatsServer(metricsUrl, metrics, {
    implementation: 'grpc-node',
    grpcJsVersion: require('@grpc/grpc-js/package.json').version,
    protoLoaderVersion: require('@grpc/proto-loader/package.json').version,
    serverConfiguration: '@grpc/grpc-js Server, default options, insecure loopback'
  });
  console.error(`[grpc-server] listening on ${url} port=${port} stats=${metricsUrl}`);
});
