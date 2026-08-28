// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createRunId,
  currentEpochNs,
  integerEnv,
  summarizeMetrics,
} = require('../common/perf_metrics');
const {
  applyContextPolicy,
  applySocketPolicy,
  appendMeasurement,
  benchmarkEndpoint,
  closeSenderWorker,
  configureTlsServer,
  drainRouterRecvInto,
  emitSingleSocketHwmDetail,
  parseSingleBinaryArgs,
  runLocalSocketOneWayBenchmark,
  spawnSenderWorker,
  waitForWorkerError,
  waitForWorkerMessage,
} = require('./perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');

const RECEIVER_ID = Buffer.from('ROUTER1', 'ascii');
const SENDER_ID = Buffer.from('ROUTER2', 'ascii');
const RECEIVER_ROUTING_ID = zlink.RoutingId.from(RECEIVER_ID);

function trace(message) {
  if (process.env.PERF_NODE_TRACE === '1') {
    console.error(`[router-router] ${message}`);
  }
}

function partStrings(received) {
  return received.parts.map((part) => part.data().toString());
}

async function handshakeRouterReceiver(receiver) {
  const ping = new zlink.Received();
  receiver.recv(ping);
  try {
    if (ping.routingId === null || partStrings(ping).join(',') !== 'PING') {
      throw new Error('router-router handshake receive failed');
    }

    await receiver.send(ping.routingId).message(Buffer.from('PONG')).submit();
    return ping.routingId;
  } finally {
    ping.close();
  }
}

async function handshakeRouterReceiverWithRetry(receiver) {
  const ping = new zlink.Received();
  try {
    const configuredMs = integerEnv('PERF_ROUTER_HANDSHAKE_TIMEOUT_MS', 3000);
    const timeoutMs = configuredMs > 0 ? configuredMs : 3000;
    const deadlineNs = process.hrtime.bigint() + BigInt(timeoutMs) * 1_000_000n;
    let received = false;
    while (!received && process.hrtime.bigint() < deadlineNs) {
      try {
        received = receiver.recv(ping, zlink.RecvFlags.DontWait);
      } catch (error) {
        if (!(error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData)) {
          throw error;
        }
      }
      if (!received) {
        await new Promise((resolve) => setTimeout(resolve, 1));
      }
    }
    if (!received) {
      throw new Error('router-router handshake ping timeout');
    }
    if (ping.routingId === null || partStrings(ping).join(',') !== 'PING') {
      throw new Error('router-router handshake receive failed');
    }

    await receiver.send(ping.routingId).message(Buffer.from('PONG')).submit();
    return ping.routingId;
  } finally {
    ping.close();
  }
}

async function runRouterRouterBenchmark(msgSize, options) {
  if (options.transport === 'inproc') {
    // inproc is context-local so the Worker sender path cannot reach it.
    // Run both ROUTER sockets in one shared context with the C-faithful
    // blocking-equivalent one-way model + the PING/PONG routing-id gate
    // (C perf_router_router.cpp does the same handshake before active).
    return runLocalSocketOneWayBenchmark({
      pattern: 'ROUTER_ROUTER',
      msgSize,
      options,
      endpointToken: 'router-router',
      createReceiver: (ctx) => zlink.createRouterSocket(ctx),
      createSender: (ctx) => zlink.createRouterSocket(ctx),
      configureReceiver: (socket) => socket.setRoutingId(RECEIVER_ROUTING_ID),
      configureSender: (socket) => socket.setRoutingId(zlink.RoutingId.from(SENDER_ID)),
      handshake: async (sender, receiver) => {
        await sender.send(RECEIVER_ROUTING_ID)
          .message(Buffer.from('PING')).submit();
        const senderRid = await handshakeRouterReceiver(receiver);
        const reply = new zlink.Received();
        sender.recv(reply);
        try {
          if (partStrings(reply).join(',') !== 'PONG') {
            throw new Error('router-router handshake reply failed');
          }
        } finally {
          reply.close();
        }
        // Receiver replies/active are addressed by the sender's routing
        // id; the sender addresses the receiver by RECEIVER_ROUTING_ID.
        return RECEIVER_ROUTING_ID;
      },
      sendActive: async (socket, payload, routingId) => {
        try {
          await appendMeasurement(socket.send(routingId), payload).submit();
          return true;
        } catch (error) {
          if (error instanceof zlink.SubmitError
            && (error.result === zlink.SubmitResult.Backpressured
              || error.result === zlink.SubmitResult.NotConnected
              || error.result === zlink.SubmitResult.NotFound)) {
            return false;
          }
          const text = String(error && error.message ? error.message : error);
          if ((error && error.code === 'EAGAIN')
            || text.includes('Resource temporarily unavailable')) {
            return false;
          }
          throw error;
        }
      },
      sendStop: async (socket, routingId) => {
        await socket.send(routingId).message(STOP_TOKEN_BYTES).submit();
      },
    });
  }

  const ctx = zlink.createContext();
  applyContextPolicy(ctx);
  const receiver = zlink.createRouterSocket(ctx);
  const endpoint = await benchmarkEndpoint(options.transport, `router-router-${msgSize}`);
  let worker = null;

  try {
    applySocketPolicy(receiver, options);
    ctx.recalculateAutoHwm();
    receiver.setRoutingId(RECEIVER_ROUTING_ID);
    configureTlsServer(receiver, options.transport);
    receiver.bind(endpoint);
    worker = spawnSenderWorker({
      kind: 'router_router',
      transport: options.transport,
      endpoint,
      duration: options.duration,
      msgSize,
      runId: options.runId ?? 1,
      receiverRoutingIdBytes: RECEIVER_ID,
      senderRoutingIdBytes: SENDER_ID,
      options,
    });
    const workerError = waitForWorkerError(worker);
    await Promise.race([
      waitForWorkerMessage(worker, 'connected'),
      workerError.then((message) => Promise.reject(new Error(message.message)))
    ]);
    worker.postMessage({ type: 'handshake' });
    await handshakeRouterReceiverWithRetry(receiver);
    await Promise.race([
      waitForWorkerMessage(worker, 'ready'),
      workerError.then((message) => Promise.reject(new Error(message.message)))
    ]);
    trace('handshake done');

    const activeStartNs = currentEpochNs();
    const activeStopNs = activeStartNs
      + BigInt(Math.floor(options.duration * 1_000_000_000));
    const runId = createRunId(options.runId ?? 1);
    const collector = createMetricCollector({
      runId,
      msgSize,
      activeStartNs,
      activeStopNs,
    });

    // PERF_SINGLE_TEST_POLICY § 1.4 / § 2.0.1: the PING/PONG handshake
    // above is the routing-id discovery gate (C perf_router_router.cpp
    // does the same). No extra start/stop control channel — the receiver
    // uses blocking recv + drain and exits on the wire stop token.
    const recvTask = drainRouterRecvInto(
      receiver,
      msgSize,
      Object.assign(collector, { runId, activeStartNs }),
      { recordUntilNs: activeStopNs }
    );
    await Promise.race([
      recvTask,
      workerError.then((message) => Promise.reject(new Error(message.message)))
    ]);
    const result = collector.finish();
    emitSingleSocketHwmDetail(receiver, 'ROUTER_ROUTER', options.transport, 'receiver', msgSize);
    return result;
  } finally {
    trace('closing');
    await closeSenderWorker(worker);
    receiver.close();
    trace('receiver closed');
    ctx.close();
    trace('ctx closed');
  }
}

module.exports = { runRouterRouterBenchmark };

if (require.main === module) {
  (async () => {
    const options = parseSingleBinaryArgs(process.argv.slice(2));
    const result = await runRouterRouterBenchmark(options.msgSize, options);
    if (result.unsupported) {
      console.log(`UNSUPPORTED,${options.libName},ROUTER_ROUTER,${options.transport}`);
      return;
    }
    for (const line of summarizeMetrics(
      'ROUTER_ROUTER',
      options.transport,
      options.msgSize,
      result.latenciesNs,
      options.duration,
      options.libName,
      result.accepted,
      result.latencyMeanNs
    )) {
      console.log(line);
    }
  })().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
