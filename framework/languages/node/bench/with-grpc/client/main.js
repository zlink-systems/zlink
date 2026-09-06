// SPDX-License-Identifier: MPL-2.0
'use strict';

// with-grpc local bench, node client. One client process, one server process per
// implementation (spec section 3). Emits `cells.json` in the `with-grpc-cell-v1` shape
// plus RESULT lines; every table, ratio and verdict is produced by
// framework/bench/tools, not here (plan section 4.1, FB-020).

require('reflect-metadata');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const zlink = require('@zlink-systems/zlink');

const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { ZLinkModule, zlinkFramework, ZLINK_ROUTE_CLIENT } = require('@zlink-systems/nestjs');
const {
  createProtobufMessageSerializer,
  ZLINK_PROTOBUF_CONTENT_TYPE
} = require('@zlink-systems/framework-codec-protobuf/framework');

const { argValue, argInt } = require('../shared/args');
const contract = require('../shared/framework-bench-contract');
const header = require('../shared/bench-metric-header');
const rawWire = require('../shared/raw-wire');
const core = require('./bench-core');

const PATTERNS = ['request-serial', 'request-window', 'send-saturation'];
const IMPLEMENTATIONS = ['grpc-node', 'zlink-node', 'zlink-framework-node'];

function parseOptions(argv) {
  const stamp = new Date().toISOString().replace(/[-:T]/g, '').slice(0, 15);
  return {
    scenario: argValue(argv, '--scenario', 'all'),
    implementation: argValue(argv, '--implementation', 'all'),
    payloadSizes: (argValue(argv, '--payload-sizes', '1024,4096')).split(',').map((v) => Number.parseInt(v, 10)),
    requestWindow: argInt(argv, '--request-window', 100),
    sendConcurrency: argInt(argv, '--send-concurrency', 8),
    latencySampleLimit: argInt(argv, '--latency-sample-limit', 200000),
    warmup: argInt(argv, '--warmup', 1000),
    durationSeconds: argInt(argv, '--duration-seconds', 5),
    commandSettleMs: argInt(argv, '--command-settle-ms', 200),
    drainBoundMs: argInt(argv, '--drain-bound-ms', 30000),
    windowSettleMs: argInt(argv, '--window-settle-ms', 5000),
    requestTimeoutMs: argInt(argv, '--request-timeout-ms', 30000),
    routeReadyMs: argInt(argv, '--route-ready-ms', 15000),
    grpcUrl: argValue(argv, '--grpc-url', '127.0.0.1:5081'),
    grpcStatsUrl: argValue(argv, '--grpc-stats-url', 'http://127.0.0.1:5084'),
    zlinkEndpoint: argValue(argv, '--zlink-endpoint', 'tcp://127.0.0.1:5082'),
    zlinkStatsUrl: argValue(argv, '--zlink-stats-url', 'http://127.0.0.1:5083'),
    zlinkRawEndpoint: argValue(argv, '--zlink-raw-endpoint', 'tcp://127.0.0.1:5085'),
    zlinkRawStatsUrl: argValue(argv, '--zlink-raw-stats-url', 'http://127.0.0.1:5086'),
    zlinkRawCommandEndpoint: argValue(argv, '--zlink-raw-command-endpoint', 'tcp://127.0.0.1:5087'),
    rawSocket: argValue(argv, '--raw-socket', process.env.RAW_SOCKET || 'router'),
    runId: (Math.floor(Math.random() * 2147483646) + 1) >>> 0,
    output: argValue(argv, '--output', 'log/latest'),
    reportFile: argValue(argv, '--report-file', `with_grpc_node_${stamp}.txt`)
  };
}

function validate(options) {
  if (options.implementation !== 'all' && !IMPLEMENTATIONS.includes(options.implementation)) {
    throw new Error(`implementation must be one of all, ${IMPLEMENTATIONS.join(', ')}`);
  }
  if (options.rawSocket !== 'router' && options.rawSocket !== 'dealer') {
    throw new Error('raw socket must be router or dealer');
  }
  for (const size of options.payloadSizes) {
    if (!Number.isFinite(size) || size < header.HEADER_SIZE) {
      throw new Error(`payload size must be at least ${header.HEADER_SIZE} bytes`);
    }
  }
  for (const url of [options.grpcStatsUrl, options.zlinkStatsUrl, options.zlinkRawStatsUrl]) {
    if (!/^http:\/\/127\.0\.0\.1:/.test(url)) throw new Error(`stats url must be loopback: ${url}`);
  }
  for (const endpoint of [options.zlinkEndpoint, options.zlinkRawEndpoint, options.zlinkRawCommandEndpoint]) {
    if (!/^tcp:\/\/127\.0\.0\.1:/.test(endpoint)) throw new Error(`endpoint must be loopback: ${endpoint}`);
  }
}

function shouldRun(options, implementation) {
  return options.implementation === 'all' || options.implementation === implementation;
}

function shouldRunPattern(options, pattern) {
  return options.scenario === 'all' || options.scenario === pattern;
}

// --- gRPC -----------------------------------------------------------------

function createGrpcClient(options) {
  const definition = protoLoader.loadSync(path.join(__dirname, '..', 'proto', 'bench.proto'), {
    keepCase: true, longs: String, enums: String, defaults: true, oneofs: true, bytes: Buffer
  });
  const proto = grpc.loadPackageDefinition(definition).zlink.framework.bench.withgrpc;
  return new proto.BenchService(options.grpcUrl, grpc.credentials.createInsecure());
}

function grpcEcho(client, options) {
  return (payloadSize, phase, sequence) => new Promise((resolve, reject) => {
    const body = header.createPayloadBytes(payloadSize, options.runId, phase, sequence);
    client.Echo({ body }, (error, reply) => {
      if (error) { reject(error); return; }
      // G2: the reply's 29-byte header is validated, not assumed.
      const decoded = header.decode(reply.body);
      if (!header.isExpected(decoded, options.runId, phase, payloadSize, sequence)) {
        reject(new Error('grpc echo reply header mismatch'));
        return;
      }
      resolve();
    });
  });
}

function grpcCommand(client, options) {
  return (payloadSize, phase, sequence) => new Promise((resolve, reject) => {
    const body = header.createPayloadBytes(payloadSize, options.runId, phase, sequence);
    // FB-002: the send comparison uses this unary Command returning Empty. gRPC has
    // no one-way primitive, so it pays a round trip for a command that needs no
    // reply; that cost belongs in a service-side comparison and is not hidden.
    client.Command({ body }, (error) => (error ? reject(error) : resolve()));
  });
}

// --- ZLink raw binding, ROUTER<->ROUTER -----------------------------------

class RawBenchSocket {
  constructor(socket, peer, isRouter) {
    this.socket = socket;
    this.peer = peer;
    this.isRouter = isRouter;
  }

  static create(ctx, mode, selfId, peerId, endpoint) {
    const peer = zlink.RoutingId.from(Buffer.from(peerId, 'ascii'));
    if (mode === 'dealer') {
      const dealer = zlink.createDealerSocket(ctx);
      dealer.setRoutingId(zlink.RoutingId.from(Buffer.from(selfId, 'ascii')));
      dealer.connect(endpoint);
      return new RawBenchSocket(dealer, peer, false);
    }
    // FB-001 / spec section 1.3: the raw row is ROUTER<->ROUTER so that
    // zlink-framework-node / zlink-node isolates framework-layer cost instead of
    // mixing in a DEALER->ROUTER socket-pattern difference.
    const router = zlink.createRouterSocket(ctx);
    router.setRoutingId(zlink.RoutingId.from(Buffer.from(selfId, 'ascii')));
    router.options.mandatory = true;
    router.options.setConnectRoutingId(peer);
    router.connect(endpoint);
    return new RawBenchSocket(router, peer, true);
  }

  request() {
    return this.isRouter ? this.socket.request(this.peer) : this.socket.request();
  }

  send() {
    return this.isRouter ? this.socket.send(this.peer) : this.socket.send();
  }

  close() {
    this.socket.close();
  }
}

function rawRequest(socket, options) {
  return async (payloadSize, phase, sequence) => {
    const payload = header.createPayloadBytes(payloadSize, options.runId, phase, sequence);
    const parts = await socket.request()
      .message(rawWire.REQUEST_ENVELOPE)
      .message(rawWire.encodeBenchPayload(payload))
      .timeout(options.requestTimeoutMs)
      .submit();
    try {
      if (parts.length === 0) throw new Error('raw request returned no reply parts');
      const body = rawWire.decodeBenchPayloadBody(parts[parts.length - 1].data());
      const decoded = header.decode(body);
      if (!header.isExpected(decoded, options.runId, phase, payloadSize, sequence)) {
        throw new Error('raw reply header mismatch');
      }
    } finally {
      for (const part of parts) part.close();
    }
  };
}

function rawSend(socket, options) {
  return async (payloadSize, phase, sequence) => {
    const payload = header.createPayloadBytes(payloadSize, options.runId, phase, sequence);
    await socket.send()
      .message(rawWire.REQUEST_ENVELOPE)
      .message(rawWire.encodeBenchPayload(payload))
      .submit();
  };
}

// --- ZLink framework, RouteMesh channel client ----------------------------

/**
 * spec section 8.1: the framework row uses `packages/framework` through its public host,
 * `@zlink-systems/nestjs`, with the protobuf codec from
 * `packages/framework-codec-protobuf`. `src/internal.ts` is not exported by the
 * package and is not touched (G4).
 */
async function createFrameworkClient(options) {
  class BenchClientModule {}
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
            .listen('tcp://127.0.0.1:0')
            .routingId(contract.CLIENT_ROUTING_ID);
          mesh.channel(contract.CHANNEL_NAME).client();
          mesh.peerConnections().connect(options.zlinkEndpoint);
          return builder.build();
        }
      })
    ]
  })(BenchClientModule);

  // Declaration gap check, before anything is measured.
  //
  // spec section 2 fixes the payload as the size of the protobuf `bytes body`. The node
  // framework codec (`packages/framework-codec-protobuf`) is a dynamic value wire
  // with no bytes type: `boolean`, `number`, `string` and `object` only. A Buffer
  // is an `object`, so each byte becomes its own keyed entry -- a 1024-byte body
  // encodes to 20,412 bytes (19.9x) and decodes back as a plain object, not
  // bytes. The framework row therefore cannot carry this bench's payload through
  // its public codec, and no amount of harness code fixes that from outside.
  // Reaching into `packages/framework`'s `src/internal.ts` for a different codec
  // path would breach G4, so the six cells are reported unsupported with this
  // reason instead of measured against a different payload.
  const probe = Buffer.from([1, 2, 3]);
  const serializer = createProtobufMessageSerializer();
  let roundTripped = null;
  try {
    const encoded = serializer.serialize({ body: probe });
    roundTripped = serializer.deserialize(encoded, Object);
  } catch (error) {
    roundTripped = null;
  }
  if (roundTripped === null || !Buffer.isBuffer(roundTripped.body)) {
    throw new Error(
      'framework-codec-protobuf has no bytes type: a protobuf `bytes body` cannot be '
      + 'expressed through the public codec (a 1024-byte body encodes to 20,412 bytes '
      + 'and decodes as an object). spec section 2 fixes the payload as that field, so this row '
      + 'cannot carry the spec payload'
    );
  }

  const app = await NestFactory.createApplicationContext(BenchClientModule, {
    logger: false,
    abortOnError: false
  });
  return { app, routeClient: app.get(ZLINK_ROUTE_CLIENT, { strict: false }) };
}

function frameworkRequest(routeClient, options) {
  return async (payloadSize, phase, sequence) => {
    const body = header.createPayloadBytes(payloadSize, options.runId, phase, sequence);
    const reply = await routeClient
      .requestToChannel(contract.CHANNEL_NAME, new contract.BenchPayload(body))
      .timeout(options.requestTimeoutMs)
      .submit();
    const replyBody = reply && reply.body;
    const decoded = header.decode(
      Buffer.isBuffer(replyBody) ? replyBody : replyBody ? Buffer.from(replyBody) : null
    );
    if (!header.isExpected(decoded, options.runId, phase, payloadSize, sequence)) {
      throw new Error('framework reply header mismatch');
    }
  };
}

function frameworkSend(routeClient, options) {
  return async (payloadSize, phase, sequence) => {
    const body = header.createPayloadBytes(payloadSize, options.runId, phase, sequence);
    await routeClient
      .sendToChannel(contract.CHANNEL_NAME, new contract.BenchPayload(body))
      .submit();
  };
}

// --- cell driving ---------------------------------------------------------

function implementationOf(cellName) {
  for (const pattern of PATTERNS) {
    if (cellName.endsWith(`-${pattern}`)) return cellName.slice(0, -(pattern.length + 1));
  }
  return cellName;
}

class DrainState {
  constructor() {
    this.pending = new Map();
    this.observations = [];
  }

  record(implementation, cellName, drainMs, boundHit, boundMs) {
    this.observations.push(
      `${cellName}: drain_ms=${drainMs.toFixed(0)} bound_hit=${boundHit} bound_ms=${boundMs}`
    );
    if (boundHit) {
      this.pending.set(
        implementation,
        `previous cell ${cellName} did not drain within ${boundMs}ms (observed ${drainMs.toFixed(0)}ms)`
      );
    }
  }

  takeContamination(implementation) {
    const reason = this.pending.get(implementation);
    if (reason === undefined) return null;
    this.pending.delete(implementation);
    return reason;
  }
}

async function main() {
  const options = parseOptions(process.argv.slice(2));
  validate(options);
  fs.mkdirSync(options.output, { recursive: true });

  const grpcClient = createGrpcClient(options);
  const ctx = zlink.createContext();

  // The framework host is built once, outside every measured window. A failure
  // to stand it up marks the six framework cells unsupported with the reason
  // rather than taking the run down.
  let framework = null;
  let frameworkError = null;
  if (shouldRun(options, 'zlink-framework-node')) {
    try {
      framework = await createFrameworkClient(options);
    } catch (error) {
      frameworkError = error && error.message;
      process.stderr.write(`[bench] framework client unavailable: ${frameworkError}\n`);
    }
  }
  const cells = [];
  const failures = [];
  const contaminated = [];
  const drain = new DrainState();

  // Cell isolation: one cell that throws must not take the other 17 with it.
  // No retry and no fabricated value -- a failed cell is simply absent, which
  // the aggregator reads as `unsupported`.
  const addCell = async (implementation, pattern, payloadSize, body) => {
    const name = `${implementation}-${pattern}`;
    const reason = drain.takeContamination(implementation);
    if (reason !== null) {
      contaminated.push(`${name}@${payloadSize}: ${reason}`);
      process.stderr.write(`[bench] CONTAMINATED ${name}@${payloadSize}: ${reason}\n`);
      return;
    }
    process.stderr.write(`[bench] running ${name}@${payloadSize}\n`);
    try {
      const cell = await body();
      cells.push({
        implementation,
        pattern,
        payload_size: payloadSize,
        contaminated: false,
        contamination_reason: null,
        ...cell
      });
      if (cell.drain_ms !== undefined) {
        drain.record(implementation, `${name}@${payloadSize}`, cell.drain_ms, cell.drain_bound_hit, options.drainBoundMs);
      }
      process.stderr.write(
        `[bench] finished ${name}@${payloadSize} throughput=${cell.throughput_per_second.toFixed(1)}/s`
        + ` cores=${cell.client_cores.toFixed(2)} peak_in_flight=${cell.peak_in_flight}\n`
      );
    } catch (error) {
      failures.push(`${name}@${payloadSize}: ${error && error.message}`);
      process.stderr.write(`[bench] FAILED ${name}@${payloadSize}: ${error && error.message}\n`);
    }
  };

  for (const payloadSize of options.payloadSizes) {
    // --- request-serial ---
    if (shouldRunPattern(options, 'request-serial')) {
      if (shouldRun(options, 'grpc-node')) {
        await addCell('grpc-node', 'request-serial', payloadSize, () => core.runRequestSerial({
          payloadSize, options, statsUrl: options.grpcStatsUrl, operation: grpcEcho(grpcClient, options)
        }));
      }
      if (shouldRun(options, 'zlink-node')) {
        await addCell('zlink-node', 'request-serial', payloadSize, async () => {
          const socket = RawBenchSocket.create(
            ctx, options.rawSocket, `bench-req-${process.pid}-s${payloadSize}`,
            rawWire.ROUTING_IDS.rawRequestServer, options.zlinkRawEndpoint
          );
          const operation = rawRequest(socket, options);
          try {
            await core.waitForRouteReady(
              () => operation(payloadSize, header.PHASE_WARMUP, 0), options.routeReadyMs
            );
            return await core.runRequestSerial({
              payloadSize, options, statsUrl: options.zlinkRawStatsUrl, operation
            });
          } finally {
            socket.close();
          }
        });
      }

      if (shouldRun(options, 'zlink-framework-node')) {
        await addCell('zlink-framework-node', 'request-serial', payloadSize, async () => {
          if (framework === null) {
            throw new Error(
              `framework host unavailable via @zlink-systems/nestjs: ${frameworkError}`
            );
          }
          const operation = frameworkRequest(framework.routeClient, options);
          await core.waitForRouteReady(
            () => operation(payloadSize, header.PHASE_WARMUP, 0), options.routeReadyMs
          );
          return core.runRequestSerial({
            payloadSize, options, statsUrl: options.zlinkStatsUrl, operation
          });
        });
      }
    }

    // --- request-window ---
    if (shouldRunPattern(options, 'request-window')) {
      if (shouldRun(options, 'grpc-node')) {
        await addCell('grpc-node', 'request-window', payloadSize, () => core.runRequestWindow({
          payloadSize, options, statsUrl: options.grpcStatsUrl, operation: grpcEcho(grpcClient, options)
        }));
      }
      if (shouldRun(options, 'zlink-node')) {
        await addCell('zlink-node', 'request-window', payloadSize, async () => {
          const socket = RawBenchSocket.create(
            ctx, options.rawSocket, `bench-win-${process.pid}-s${payloadSize}`,
            rawWire.ROUTING_IDS.rawRequestServer, options.zlinkRawEndpoint
          );
          const operation = rawRequest(socket, options);
          try {
            await core.waitForRouteReady(
              () => operation(payloadSize, header.PHASE_WARMUP, 0), options.routeReadyMs
            );
            return await core.runRequestWindow({
              payloadSize, options, statsUrl: options.zlinkRawStatsUrl, operation
            });
          } finally {
            socket.close();
          }
        });
      }

      if (shouldRun(options, 'zlink-framework-node')) {
        await addCell('zlink-framework-node', 'request-window', payloadSize, async () => {
          if (framework === null) {
            throw new Error(
              `framework host unavailable via @zlink-systems/nestjs: ${frameworkError}`
            );
          }
          const operation = frameworkRequest(framework.routeClient, options);
          await core.waitForRouteReady(
            () => operation(payloadSize, header.PHASE_WARMUP, 0), options.routeReadyMs
          );
          return core.runRequestWindow({
            payloadSize, options, statsUrl: options.zlinkStatsUrl, operation
          });
        });
      }
    }

    // --- send-saturation ---
    if (shouldRunPattern(options, 'send-saturation')) {
      if (shouldRun(options, 'grpc-node')) {
        await addCell('grpc-node', 'send-saturation', payloadSize, () => core.runSendSaturation({
          payloadSize, options, statsUrl: options.grpcStatsUrl, operation: grpcCommand(grpcClient, options)
        }));
      }
      if (shouldRun(options, 'zlink-node')) {
        await addCell('zlink-node', 'send-saturation', payloadSize, async () => {
          const socket = RawBenchSocket.create(
            ctx, options.rawSocket, `bench-send-${process.pid}-s${payloadSize}`,
            rawWire.ROUTING_IDS.rawCommandServer, options.zlinkRawCommandEndpoint
          );
          const operation = rawSend(socket, options);
          try {
            await core.waitForRouteReady(
              () => operation(payloadSize, header.PHASE_WARMUP, 0), options.routeReadyMs
            );
            return await core.runSendSaturation({
              payloadSize, options, statsUrl: options.zlinkRawStatsUrl, operation
            });
          } finally {
            socket.close();
          }
        });
      }

      if (shouldRun(options, 'zlink-framework-node')) {
        await addCell('zlink-framework-node', 'send-saturation', payloadSize, async () => {
          if (framework === null) {
            throw new Error(
              `framework host unavailable via @zlink-systems/nestjs: ${frameworkError}`
            );
          }
          const operation = frameworkSend(framework.routeClient, options);
          await core.waitForRouteReady(
            () => operation(payloadSize, header.PHASE_WARMUP, 0), options.routeReadyMs
          );
          return core.runSendSaturation({
            payloadSize, options, statsUrl: options.zlinkStatsUrl, operation
          });
        });
      }
    }
  }

  await writeOutputs(options, cells, failures, contaminated, drain);
  process.stderr.write(
    `[bench] cells completed=${cells.length} failed=${failures.length} contaminated=${contaminated.length}\n`
  );
  ctx.close();
  grpc.closeClient(grpcClient);
  if (framework !== null) await framework.app.close();
}

async function writeOutputs(options, cells, failures, contaminated, drain) {
  const metadata = await collectMetadata(options, contaminated);
  fs.writeFileSync(
    path.join(options.output, 'cells.json'),
    `${JSON.stringify({ schema: 'with-grpc-cell-v1', metadata, cells }, null, 2)}\n`
  );

  const lines = [];
  lines.push('# with-grpc bench, node row');
  for (const [key, value] of Object.entries(metadata)) {
    if (typeof value === 'object') continue;
    lines.push(`# ${key}: ${value}`);
  }
  lines.push('');
  const metricOf = (cell) => ({
    throughput: cell.throughput_per_second,
    bandwidth: cell.bandwidth_mb_s,
    latency: cell.latency_mean_ms,
    latency_p95: cell.latency_p95_ms,
    latency_p99: cell.latency_p99_ms,
    client_cpu_percent: cell.client_cpu_percent,
    client_memory_mb: cell.client_memory_mb,
    server_cpu_percent: cell.server_cpu_percent,
    server_memory_mb: cell.server_memory_mb
  });
  for (const cell of cells) {
    const scenario = `${cell.implementation}-${cell.pattern}`;
    for (const [metric, value] of Object.entries(metricOf(cell))) {
      lines.push(`RESULT,current,${scenario},local,${cell.payload_size},${metric},${value.toFixed(3)}`);
    }
  }
  fs.writeFileSync(path.join(options.output, options.reportFile), `${lines.join('\n')}\n`);
  fs.writeFileSync(path.join(options.output, 'report.txt'), `${lines.join('\n')}\n`);

  if (failures.length > 0 || contaminated.length > 0 || drain.observations.length > 0) {
    const summary = [];
    if (drain.observations.length > 0) {
      summary.push('## Drain (FB-008)');
      for (const line of drain.observations) summary.push(`- ${line}`);
      summary.push('');
    }
    if (contaminated.length > 0) {
      summary.push('## Contaminated (excluded from tables and judgement)');
      for (const line of contaminated) summary.push(`- ${line}`);
      summary.push('');
    }
    summary.push('## Failures');
    for (const line of failures) summary.push(`- ${line}`);
    fs.writeFileSync(path.join(options.output, 'failures.txt'), `${summary.join('\n')}\n`);
    process.stderr.write(`${summary.join('\n')}\n`);
  }
}

function readZlinkBindingVersion() {
  // The binding's package.json is not an exported subpath, so it is read from
  // the resolved module directory rather than required.
  try {
    const entry = require.resolve('@zlink-systems/zlink');
    let dir = path.dirname(entry);
    for (let i = 0; i < 6; i++) {
      const candidate = path.join(dir, 'package.json');
      if (fs.existsSync(candidate)) return JSON.parse(fs.readFileSync(candidate, 'utf8')).version;
      dir = path.dirname(dir);
    }
  } catch (error) { /* provenance is best effort */ }
  return 'unknown';
}

async function collectMetadata(options, contaminated) {
  let commit = 'unknown';
  try {
    commit = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
  } catch (error) { /* provenance is best effort; the run still reports */ }
  let loadavg = os.loadavg();
  let grpcInfo = {};
  try {
    grpcInfo = await (await fetch(`${options.grpcStatsUrl}/bench/info`)).json();
  } catch (error) { /* server may not expose info */ }
  return {
    diagnosticsSchema: 'with-grpc-cell-v1',
    language: 'node',
    nodeVersion: process.version,
    grpcJsVersion: require('@grpc/grpc-js/package.json').version,
    protoLoaderVersion: require('@grpc/proto-loader/package.json').version,
    zlinkBindingVersion: readZlinkBindingVersion(),
    grpcServerConfiguration: grpcInfo.serverConfiguration || '@grpc/grpc-js Server, default options',
    logical_cores: core.LOGICAL_CORES,
    client_saturation_metric: core.CLIENT_SATURATION_METRIC,
    client_parallelism_ceiling: core.CLIENT_PARALLELISM_CEILING,
    cpu: os.cpus()[0] ? os.cpus()[0].model : 'unknown',
    kernel: os.release(),
    commit,
    rawSocket: options.rawSocket,
    warmup: options.warmup,
    durationSeconds: options.durationSeconds,
    requestWindow: options.requestWindow,
    sendConcurrency: options.sendConcurrency,
    runId: options.runId,
    startedAt: new Date().toISOString(),
    loadavg1: loadavg[0],
    contaminatedCells: contaminated
  };
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
