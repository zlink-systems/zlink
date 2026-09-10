// SPDX-License-Identifier: MPL-2.0
'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const zlink = require('@zlink-systems/zlink');

const { argValue, argInt } = require('../shared/args');
const { BenchPhaseController, startSourceHttp } = require('../shared/bench-http-application');
const header = require('../shared/bench-metric-header');
const rawWire = require('../shared/raw-wire');
const core = require('./bench-core');
const { createFrameworkTransport } = require('./framework-transport');

const PATTERNS = ['request-serial', 'request-window', 'request-backpressure', 'send-saturation'];
const IMPLEMENTATIONS = ['grpc-node', 'zlink-node', 'zlink-framework-node'];

function parseOptions(argv) {
  return {
    implementation: argValue(argv, '--implementation', ''),
    scenario: argValue(argv, '--scenario', ''),
    payloadSize: argInt(argv, '--payload-size', 1024),
    requestWindow: argInt(argv, '--request-window', 100),
    sendConcurrency: argInt(argv, '--send-concurrency', 8),
    latencySampleLimit: argInt(argv, '--latency-sample-limit', 200000),
    warmup: argInt(argv, '--warmup', 1000),
    drainBoundMs: argInt(argv, '--drain-bound-ms', 30000),
    requestTimeoutMs: argInt(argv, '--request-timeout-ms', 30000),
    routeReadyMs: argInt(argv, '--route-ready-ms', 30000),
    triggerUrl: argValue(argv, '--trigger-url', ''),
    statsUrl: argValue(argv, '--stats-url', ''),
    targetEndpoint: argValue(argv, '--target-endpoint', ''),
    targetCommandEndpoint: argValue(argv, '--target-command-endpoint', ''),
    targetStatsUrl: argValue(argv, '--target-stats-url', ''),
    rawSocket: argValue(argv, '--raw-socket', process.env.RAW_SOCKET || 'router'),
    output: argValue(argv, '--output', 'log/latest'),
    reportFile: argValue(argv, '--report-file', 'report.txt')
  };
}

function validateOptions(options) {
  if (!IMPLEMENTATIONS.includes(options.implementation)) throw new Error('unknown implementation');
  if (!PATTERNS.includes(options.scenario)) throw new Error('unknown scenario');
  if (options.payloadSize < header.HEADER_SIZE || options.requestWindow !== 100
      || options.sendConcurrency !== 8 || options.warmup < 0
      || options.latencySampleLimit <= 0 || options.drainBoundMs !== 30000
      || options.requestTimeoutMs !== 30000 || options.routeReadyMs !== 30000) {
    throw new Error('fixed benchmark options are invalid');
  }
  if (options.rawSocket !== 'router') throw new Error('RAW_SOCKET must remain router');
  validateEndpoint(options.triggerUrl, 'http:', 'trigger URL');
  validateEndpoint(options.statsUrl, 'http:', 'stats URL');
  validateEndpoint(options.targetStatsUrl, 'http:', 'target stats URL');
  validateEndpoint(
    options.targetEndpoint,
    options.implementation === 'grpc-node' ? 'grpc:' : 'tcp:',
    'target endpoint'
  );
  if (options.implementation === 'zlink-node') {
    validateEndpoint(options.targetCommandEndpoint, 'tcp:', 'target command endpoint');
  }
}

function validateEndpoint(value, scheme, name) {
  const parsed = scheme === 'grpc:' ? new URL(`grpc://${value}`) : new URL(value);
  if (parsed.protocol !== scheme || parsed.hostname !== '127.0.0.1') {
    throw new Error(`${name} must use ${scheme}//127.0.0.1`);
  }
}

function validateTriggerForCell(trigger, options) {
  if (trigger.pattern !== options.scenario || trigger.payloadBytes !== options.payloadSize
      || trigger.requestWindow !== options.requestWindow
      || trigger.sendConcurrency !== options.sendConcurrency) {
    throw new Error('trigger values do not match the runner-owned cell configuration');
  }
}

function grpcCall(client, method, body, timeoutMs) {
  return new Promise((resolve, reject) => {
    client[method]({ body }, { deadline: Date.now() + timeoutMs }, (error, reply) => {
      if (error) reject(error);
      else resolve(reply);
    });
  });
}

function createGrpcTransport(options) {
  const definition = protoLoader.loadSync(path.join(__dirname, '..', '..', 'proto', 'bench.proto'), {
    keepCase: true, longs: String, enums: String, defaults: true, oneofs: true, bytes: Buffer
  });
  const proto = grpc.loadPackageDefinition(definition).zlink.framework.bench.withgrpc;
  const streamCount = options.scenario === 'send-saturation' ? options.sendConcurrency : 1;
  const clients = Array.from({ length: streamCount }, () => new proto.BenchService(
    options.targetEndpoint, grpc.credentials.createInsecure()
  ));
  return {
    request: async (stream, payload) => {
      const reply = await grpcCall(
        clients[stream % clients.length], 'Echo', payload, options.requestTimeoutMs
      );
      return reply.body;
    },
    send: async (stream, payload) => {
      await grpcCall(clients[stream % clients.length], 'Command', payload, options.requestTimeoutMs);
    },
    close: async () => {
      for (const client of clients) grpc.closeClient(client);
    }
  };
}

class RawBenchSocket {
  constructor(socket, peer) {
    this.socket = socket;
    this.peer = peer;
  }

  static create(context, selfId, peerId, endpoint) {
    const socket = zlink.createRouterSocket(context);
    const peer = zlink.RoutingId.from(Buffer.from(peerId, 'ascii'));
    socket.setRoutingId(zlink.RoutingId.from(Buffer.from(selfId, 'ascii')));
    socket.options.mandatory = true;
    socket.options.setConnectRoutingId(peer);
    socket.connect(endpoint);
    return new RawBenchSocket(socket, peer);
  }

  close() {
    this.socket.close();
  }
}

function createRawTransport(options) {
  const context = zlink.createContext();
  const requestSocket = options.scenario === 'send-saturation' ? null : RawBenchSocket.create(
    context,
    `bench-source-request-${process.pid}`,
    rawWire.ROUTING_IDS.rawRequestServer,
    options.targetEndpoint
  );
  const sendSockets = options.scenario === 'send-saturation'
    ? Array.from({ length: options.sendConcurrency }, (_, index) => RawBenchSocket.create(
      context,
      `bench-source-send-${process.pid}-${index}`,
      rawWire.ROUTING_IDS.rawCommandServer,
      options.targetCommandEndpoint
    ))
    : [];
  return {
    request: async (_stream, payload) => {
      const parts = await requestSocket.socket.request(requestSocket.peer)
        .message(rawWire.REQUEST_ENVELOPE)
        .message(rawWire.encodeBenchPayloadMessage(payload))
        .timeout(options.requestTimeoutMs)
        .submit();
      try {
        if (parts.length === 0) throw new Error('raw request returned no reply parts');
        const body = rawWire.decodeBenchPayloadBody(parts[parts.length - 1].data());
        if (body === null) throw new Error('raw reply protobuf body is invalid');
        return body;
      } finally {
        for (const part of parts) part.close();
      }
    },
    send: async (stream, payload) => {
      const socket = sendSockets[stream % sendSockets.length];
      await socket.socket.send(socket.peer)
        .message(rawWire.REQUEST_ENVELOPE)
        .message(rawWire.encodeBenchPayloadMessage(payload))
        .submit();
    },
    close: async () => {
      if (requestSocket !== null) requestSocket.close();
      for (const socket of sendSockets) socket.close();
      context.close();
    }
  };
}

async function createTransport(options) {
  const transport = options.implementation === 'grpc-node'
    ? createGrpcTransport(options)
    : options.implementation === 'zlink-framework-node'
      ? await createFrameworkTransport(options)
      : createRawTransport(options);
  const probeBody = header.createPayloadBytes(1024, 1, header.PHASE_WARMUP, 0);
  await core.waitForRouteReady(async () => {
    if (options.scenario === 'send-saturation') {
      await transport.send(0, probeBody);
      return;
    }
    const reply = await transport.request(0, probeBody);
    if (!header.isExpected(header.decode(reply), 1, header.PHASE_WARMUP, 1024, 0)) {
      throw new Error('target probe returned an invalid payload');
    }
  }, options.routeReadyMs);
  return transport;
}

async function main() {
  const options = parseOptions(process.argv.slice(2));
  validateOptions(options);
  fs.mkdirSync(options.output, { recursive: true });

  const transport = await createTransport(options);
  const metrics = new core.SourceMetrics(options.latencySampleLimit);
  let ready = true;
  let activeResult = null;
  let controller;
  controller = new BenchPhaseController(
    () => ready,
    () => metrics.snapshot(),
    async (trigger) => {
      validateTriggerForCell(trigger, options);
      if (trigger.phase === 'warmup') {
        await core.runWarmup(transport, options, trigger);
        return;
      }
      activeResult = await core.runActive(transport, metrics, options, trigger);
      await writeResult(options, controller.lastTrigger, activeResult);
    }
  );
  const httpServers = await startSourceHttp(options.triggerUrl, options.statsUrl, controller);
  process.stderr.write(
    `[source] implementation=${options.implementation} trigger=${options.triggerUrl}`
    + ` stats=${options.statsUrl} target=${options.targetEndpoint}\n`
  );

  let stopping = false;
  const stop = async () => {
    if (stopping) return;
    stopping = true;
    ready = false;
    await httpServers.close();
    await transport.close();
  };
  process.on('SIGTERM', () => { stop().then(() => process.exit(0)); });
  process.on('SIGINT', () => { stop().then(() => process.exit(130)); });
}

async function writeResult(options, observedTrigger, result) {
  const trigger = {
    runId: observedTrigger.runId,
    cellId: observedTrigger.cellId,
    pattern: observedTrigger.pattern,
    payloadBytes: observedTrigger.payloadBytes,
    durationMs: observedTrigger.durationMs,
    warmup: options.warmup,
    endpoint: `${options.triggerUrl}/bench/start`,
    receivedAtUnixMs: observedTrigger.receivedAtUnixMs
  };
  const cell = {
    implementation: options.implementation,
    pattern: options.scenario,
    payload_size: options.payloadSize,
    role: 'source',
    trigger,
    streams: core.streamDescription(
      options.scenario, options.requestWindow, options.sendConcurrency
    ),
    target_stats: null,
    ...result
  };
  const metadata = await collectMetadata(options);
  fs.writeFileSync(
    path.join(options.output, 'results.json'),
    `${JSON.stringify({ schema: 'with-grpc-cell-v1', metadata, cells: [cell] }, null, 2)}\n`
  );

  const lines = [
    '# with-grpc bench, node server-driven source A',
    `# implementation: ${options.implementation}`,
    `# pattern: ${options.scenario}`,
    `# payload_size: ${options.payloadSize}`,
    `# warmup: ${options.warmup}`,
    ''
  ];
  const metrics = {
    throughput: result.throughput_per_second,
    bandwidth: result.bandwidth_mb_s,
    latency: result.latency_mean_ms,
    latency_p95: result.latency_p95_ms,
    latency_p99: result.latency_p99_ms,
    client_cpu_percent: result.client_cpu_percent,
    client_memory_mb: result.client_memory_mb,
    server_cpu_percent: result.server_cpu_percent,
    server_memory_mb: result.server_memory_mb
  };
  const scenario = `${options.implementation}-${options.scenario}`;
  for (const [name, value] of Object.entries(metrics)) {
    lines.push(`RESULT,current,${scenario},local,${options.payloadSize},${name},${value.toFixed(3)}`);
  }
  const text = `${lines.join('\n')}\n`;
  fs.writeFileSync(path.join(options.output, options.reportFile), text);
  process.stdout.write(text);
}

function readPackageVersion(packageName) {
  try {
    const entry = require.resolve(packageName);
    let directory = path.dirname(entry);
    for (let index = 0; index < 8; index++) {
      const candidate = path.join(directory, 'package.json');
      if (fs.existsSync(candidate)) return JSON.parse(fs.readFileSync(candidate, 'utf8')).version;
      directory = path.dirname(directory);
    }
  } catch (error) {
    return 'unknown';
  }
  return 'unknown';
}

async function collectMetadata(options) {
  let commit = 'unknown';
  try {
    commit = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
  } catch (error) {
    // Provenance is best effort; a missing git executable does not change the cell.
  }
  let targetInfo = {};
  try {
    const response = await fetch(`${options.targetStatsUrl}/bench/info`);
    if (response.ok) targetInfo = await response.json();
  } catch (error) {
    // The stats snapshot remains authoritative when optional info is unavailable.
  }
  return {
    diagnosticsSchema: 'with-grpc-cell-v1',
    language: 'node',
    nodeVersion: process.version,
    grpcJsVersion: require('@grpc/grpc-js/package.json').version,
    protoLoaderVersion: require('@grpc/proto-loader/package.json').version,
    zlinkBindingVersion: readPackageVersion('@zlink-systems/zlink'),
    grpcServerConfiguration: targetInfo.serverConfiguration
      || '@grpc/grpc-js Server, default options, insecure loopback',
    logicalCores: core.LOGICAL_CORES,
    clientSaturationMetric: core.CLIENT_SATURATION_METRIC,
    clientParallelismCeiling: core.CLIENT_PARALLELISM_CEILING,
    cpu: os.cpus()[0] ? os.cpus()[0].model : 'unknown',
    kernel: os.release(),
    commit,
    rawSocket: options.rawSocket,
    warmup: options.warmup,
    requestWindow: options.requestWindow,
    sendConcurrency: options.sendConcurrency,
    triggerUrl: options.triggerUrl,
    statsUrl: options.statsUrl,
    targetEndpoint: options.targetEndpoint,
    targetCommandEndpoint: options.targetCommandEndpoint || null,
    targetStatsUrl: options.targetStatsUrl,
    generatedUtc: new Date().toISOString()
  };
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
