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

const { argValue, argInt } = require('../shared/args');
const { BenchServerMetrics, startStatsServer } = require('../shared/bench-server-metrics');
const rawWire = require('../shared/raw-wire');
const { RESPONSE_ENVELOPE, ROUTING_IDS, decodeBenchPayloadBody, encodeBenchPayloadBytes } = rawWire;
const zlink = require('@zlink-systems/zlink');
const { EchoHandler, CommandHandler, BENCH_SERVER_METRICS, toBuffer } = require('./bench-handlers');
const { createReceivePermitPump } = require('./receive-permit-pump');
const ladderL5ToL7 = process.env.ZLINK_LADDER_L5_7_SERVER === '1';
const l0Server = process.env.ZLINK_LADDER_L0_SERVER === '1' && !ladderL5ToL7;
const lazyFramework = l0Server && process.env.ZLINK_LADDER_L0_LAZY_FRAMEWORK !== '0';
const l0rServer = l0Server && process.env.ZLINK_LADDER_L0R === '1';
const l2Envelope = l0Server && process.env.ZLINK_LADDER_L2_ENVELOPE === '1';
const l2rEnvelope = l0Server && process.env.ZLINK_LADDER_L2R_ENVELOPE === '1';
const l3ServiceWire = l0Server && process.env.ZLINK_LADDER_L3_SERVICE_WIRE === '1';
const l3rServiceWire = l0Server && process.env.ZLINK_LADDER_L3R_SERVICE_WIRE === '1';
const l4TypedPayload = l0Server && process.env.ZLINK_LADDER_L4_SERVER === '1';
const l3ServiceWireEnabled = l3ServiceWire || l4TypedPayload;
const l1Permit = l0Server && (process.env.ZLINK_LADDER_L1_PERMIT === '1'
  || l2Envelope || l2rEnvelope || l3ServiceWireEnabled || l3rServiceWire);
let applicationJobQueue;
let ApplicationJobQueue;
let resolveApplicationJobQueueConfiguration;
if (l0rServer || l1Permit) {
  require('@zlink-systems/framework');
  ({ ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require(
    '../../../../languages/node/packages/framework/dist/runtime/host/application-job-queue'
  ));
}
if (l1Permit) {
  applicationJobQueue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
}
if (l2Envelope || l2rEnvelope) rawWire.requireProductionChannelEnvelope();
if (l3ServiceWireEnabled || l3rServiceWire) rawWire.requireProductionServiceWire();
const frameworkWire = process.env.ZLINK_LADDER_L0_FRAMEWORK_CLIENT === '1'
  || process.env.ZLINK_CROSS_FRAMEWORK_WIRE === '1';
let contract;
let createBenchPayloadSerializer;
let Module;
let Injectable;
let Inject;
let NestFactory;
let ZLinkModule;
let zlinkFramework;
let ZLINK_PROTOBUF_CONTENT_TYPE;
if (!lazyFramework) {
  contract = require('../shared/framework-bench-contract');
  if (process.env.ZLINK_LADDER_L5_7_SERVER !== '1') {
    ({ createBenchPayloadSerializer } = require('../shared/framework-protobuf'));
  }
  require('reflect-metadata');
  ({ Module, Injectable, Inject } = require('@nestjs/common'));
  ({ NestFactory } = require('@nestjs/core'));
  ({ ZLinkModule, zlinkFramework } = require('@zlink-systems/nestjs'));
  if (process.env.ZLINK_LADDER_L5_7_SERVER !== '1') {
    ({ ZLINK_PROTOBUF_CONTENT_TYPE } = require('@zlink-systems/framework-codec-protobuf/framework'));
  }
  Injectable()(EchoHandler);
  Injectable()(CommandHandler);
  Inject(BENCH_SERVER_METRICS)(EchoHandler, undefined, 0);
  Inject(BENCH_SERVER_METRICS)(CommandHandler, undefined, 0);
}

const argv = process.argv.slice(2);
const endpoint = argValue(argv, '--endpoint', 'tcp://127.0.0.1:5234');
const metricsUrl = argValue(argv, '--metrics-url', 'http://127.0.0.1:5235');
const batch = argInt(argv, '--recv-batch', 256);

const metrics = new BenchServerMetrics();

if (l0Server) {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const twoRouters = process.env.ZLINK_LADDER_L0_TWO_ROUTERS !== '0';
  const commandRouter = twoRouters ? zlink.createRouterSocket(ctx) : null;
  router.setRoutingId(zlink.RoutingId.from(Buffer.from(
    frameworkWire ? 'bench-server' : ROUTING_IDS.rawRequestServer,
    'ascii'
  )));
  router.options.mandatory = true;
  if (commandRouter !== null) {
    commandRouter.setRoutingId(zlink.RoutingId.from(Buffer.from(ROUTING_IDS.rawCommandServer, 'ascii')));
    commandRouter.options.mandatory = true;
  }
  router.bind(endpoint);
  if (commandRouter !== null) commandRouter.bind(argValue(argv, '--command-endpoint', 'tcp://127.0.0.1:5236'));
  startStatsServer(metricsUrl, metrics, { implementation: 'zlink-framework-node-l0', endpoint });
  const received = new zlink.Received();
  const commandReceived = commandRouter === null ? null : new zlink.Received();
  const echoHandler = new EchoHandler(metrics);
  const commandHandler = new CommandHandler(metrics);
  const IDLE_SPIN_LIMIT = 20000;
  let idleSpins = 0;

  function decodeLadderParts(targetReceived) {
    // L0/L1 consume envelope bytes like the raw terminal reader. L2's header
    // decoder and L3's service decoder already read their envelope parts.
    if (!l2Envelope && !l3ServiceWireEnabled) targetReceived.parts[0]?.data();
    return l3ServiceWireEnabled
      ? rawWire.decodeProductionServiceRequest(
          targetReceived.parts,
          targetReceived.replyToken !== null
        )
      : { correlation: null, parts: targetReceived.parts };
  }

  function closeLadderParts(decoded) {
    if (l3ServiceWireEnabled) {
      for (const part of decoded.parts) part.close();
    }
  }

  function ladderReplyParts(channelHeader, payload) {
    if (l4TypedPayload) {
      return rawWire.encodeProductionTypedChannelReply(channelHeader, payload);
    }
    return l2Envelope || l3ServiceWireEnabled
      ? rawWire.encodeProductionProtobufChannelReply(channelHeader, payload)
      : [RESPONSE_ENVELOPE, encodeBenchPayloadBytes(payload)];
  }

  function submitLadderReply(targetReceived, decoded, payload, channelHeader) {
    const replyParts = ladderReplyParts(channelHeader, payload);
    if (l3ServiceWireEnabled) {
      const reply = rawWire.encodeProductionServiceReply(decoded.correlation, replyParts);
      const frame = reply.frame.takeMessage();
      try {
        targetReceived.reply().message(reply.header).message(frame).submit();
      } catch (error) {
        frame.close();
        throw error;
      }
      return;
    }
    targetReceived.reply().message(replyParts[0]).message(replyParts[1]).submit();
  }

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
      let decoded;
      try {
        decoded = decodeLadderParts(received);
        const parts = decoded.parts;
        const channelHeader = l2Envelope || l3ServiceWireEnabled
          ? rawWire.decodeProductionChannelHeader(parts)
          : null;
        const header = frameworkWire ? parts[0].data() : null;
        const typedPayload = l4TypedPayload
          ? rawWire.decodeProductionTypedPayload(parts[parts.length - 1].data(), channelHeader)
          : null;
        const payload = l4TypedPayload
          ? toBuffer(typedPayload.body)
          : decodeBenchPayloadBody(parts[parts.length - 1].data());
        if (payload === null) throw new Error('invalid benchmark payload');
        if (received.replyToken !== null) {
          echoHandler.handleL0(payload);
          if (l3ServiceWireEnabled) submitLadderReply(received, decoded, l4TypedPayload ? typedPayload : payload, channelHeader);
          else {
            const reply = l2Envelope
              ? ladderReplyParts(channelHeader, payload)
              : [frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE,
                encodeBenchPayloadBytes(payload)];
            received.reply().message(reply[0]).message(reply[1]).submit();
          }
        } else {
          commandHandler.handleL0(payload);
        }
      } catch (error) {
        metrics.recordError();
        console.error(`ladder L0 handler failed: ${error.message}`);
      } finally {
        if (decoded !== undefined) closeLadderParts(decoded);
      }
    }
    return handled;
  }

  function pumpCommands() {
    if (commandRouter === null) return 0;
    let handled = 0;
    while (handled < batch) {
      let hasMessage;
      try {
        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
      } catch (error) {
        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
        metrics.recordError();
        console.error(`ladder L0 command recv failed: ${error.message}`);
        break;
      }
      if (!hasMessage) break;
      handled += 1;
      let decoded;
      try {
        decoded = decodeLadderParts(commandReceived);
        const parts = decoded.parts;
        const channelHeader = l2Envelope || l3ServiceWireEnabled
          ? rawWire.decodeProductionChannelHeader(parts)
          : null;
        const typedPayload = l4TypedPayload
          ? rawWire.decodeProductionTypedPayload(parts[parts.length - 1].data(), channelHeader)
          : null;
        const payload = l4TypedPayload
          ? toBuffer(typedPayload.body)
          : decodeBenchPayloadBody(parts[parts.length - 1].data());
        if (payload === null) throw new Error('invalid benchmark payload');
        commandHandler.handleL0(payload);
      } catch (error) {
        metrics.recordError();
        console.error(`ladder L0 command handler failed: ${error.message}`);
      } finally {
        if (decoded !== undefined) closeLadderParts(decoded);
      }
    }
    return handled;
  }

  const permitStop = new AbortController();
  function permittedPump(socket, targetReceived, processMessage, label) {
    if (!l1Permit || socket === null) return () => 0;
    return createReceivePermitPump(applicationJobQueue,
      () => socket.recv(targetReceived, zlink.RecvFlags.DontWait),
      processMessage,
      (error, phase) => {
        if (phase === 'recv' && error instanceof zlink.RecvError
          && error.result === zlink.RecvResult.NoData) return;
        metrics.recordError();
        console.error(`ladder L1 ${label} ${phase} failed: ${error.message}`);
      }, permitStop.signal, batch);
  }

  const pumpRequestsWithPermits = permittedPump(router, received, (permit) => {
    const decoded = decodeLadderParts(received);
    const parts = decoded.parts;
    try {
      const channelHeader = l2Envelope || l3ServiceWireEnabled
        ? rawWire.decodeProductionChannelHeader(parts)
        : null;
      const typedPayload = l4TypedPayload
        ? rawWire.decodeProductionTypedPayload(parts[parts.length - 1].data(), channelHeader)
        : null;
      const payload = l4TypedPayload
        ? toBuffer(typedPayload.body)
        : decodeBenchPayloadBody(parts[parts.length - 1].data());
      if (payload === null) throw new Error('invalid benchmark payload');
      if (received.replyToken !== null) {
        permit.releaseBeforeHandler();
        echoHandler.handleL0(payload);
        submitLadderReply(received, decoded, l4TypedPayload ? typedPayload : payload, channelHeader);
      } else {
        permit.releaseBeforeHandler();
        commandHandler.handleL0(payload);
      }
    } finally {
      closeLadderParts(decoded);
    }
  }, 'request');

  const pumpCommandsWithPermits = permittedPump(commandRouter, commandReceived, (permit) => {
    const decoded = decodeLadderParts(commandReceived);
    const parts = decoded.parts;
    try {
      const channelHeader = l2Envelope || l3ServiceWireEnabled
        ? rawWire.decodeProductionChannelHeader(parts)
        : null;
      const typedPayload = l4TypedPayload
        ? rawWire.decodeProductionTypedPayload(parts[parts.length - 1].data(), channelHeader)
        : null;
      const payload = l4TypedPayload
        ? toBuffer(typedPayload.body)
        : decodeBenchPayloadBody(parts[parts.length - 1].data());
      if (payload === null) throw new Error('invalid benchmark payload');
      permit.releaseBeforeHandler();
      commandHandler.handleL0(payload);
    } finally {
      closeLadderParts(decoded);
    }
  }, 'command');

  function schedulePermittedPump() {
    const handled = pumpRequestsWithPermits() + pumpCommandsWithPermits();
    if (handled > 0) {
      idleSpins = 0;
      setImmediate(schedulePump);
      return;
    }
    idleSpins += 1;
    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
    else setTimeout(schedulePump, 1);
  }

  function schedulePump() {
    if (l1Permit) {
      schedulePermittedPump();
      return;
    }
    const handled = twoRouters ? pump() + pumpCommands() : pump();
    if (handled > 0) {
      idleSpins = 0;
      setImmediate(schedulePump);
      return;
    }
    idleSpins += 1;
    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
    else setTimeout(schedulePump, 1);
  }

  process.stderr.write(`[framework-server-l0] endpoint=${endpoint} stats=${metricsUrl}\n`);
  if (l1Permit) {
    process.stderr.write(`[framework-server-l0] permit_path=production-receive-rotation budget=${batch}\n`);
  }
  process.on('SIGTERM', () => {
    permitStop.abort();
    router.close();
    if (commandRouter !== null) commandRouter.close();
    ctx.close();
    process.exit(0);
  });
  setImmediate(schedulePump);
} else {
class BenchServerModule {}
Module({
  imports: [
    ZLinkModule.forRootFactory({
      inject: [],
      useFactory: () => {
        const builder = zlinkFramework();
          if (process.env.ZLINK_LADDER_L5_7_SERVER !== '1') {
            builder.codecs().use({
              register: (codecs) => {
                codecs.addSerializer(ZLINK_PROTOBUF_CONTENT_TYPE, createBenchPayloadSerializer());
              }
            });
          }
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
    { provide: BENCH_SERVER_METRICS, useValue: metrics }
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
    codec: ladderL5ToL7 ? 'framework-json-v1' : 'framework-codec-protobuf',
    endpoint
  });
  process.stderr.write(`[framework-server] endpoint=${endpoint} stats=${metricsUrl}\n`);
  process.on('SIGTERM', async () => { await app.close(); process.exit(0); });
})().catch((error) => {
  console.error(`framework server failed: ${error && error.stack}`);
  process.exit(1);
});
}
