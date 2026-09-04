// SPDX-License-Identifier: MPL-2.0

'use strict';

const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createPayload,
  createRunId,
  HEADER_SIZE,
  currentEpochNs,
  sleepImmediate,
  stampPayload,
  summarizeMetrics
} = require('../common/perf_metrics');
const { configureTlsClient, configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const {
  POLLIN,
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  measurementParts,
  measurementPayload,
  pollEvents,
  pollEventHas,
  recvNoWaitInto,
  sendRouted,
  waitForConnectionReady,
  waitForConnectionReadyCount,
  waitPollerOne
} = require('./perf_multi_runtime');

const SERVER_ROUTING_ID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
const ASYNC_PROGRESS_BATCH = 64;

function resolveRoutedPattern(pattern, family) {
  const base = `MULTI_${family}`;
  const normalized = String(pattern || `${base}_REQREP`).trim().toUpperCase();
  if (normalized === base || normalized === `${base}_SENDSEND`) {
    return `${base}_SENDSEND`;
  }
  return `${base}_REQREP`;
}

function createClientSocket(ctx, routerClient) {
  return routerClient
    ? zlink.createRouterSocket(ctx)
    : zlink.createDealerSocket(ctx);
}

function sendPayload(socket, routerClient, payload) {
  return routerClient
    ? sendRouted(socket, SERVER_ROUTING_ID, payload)
    : sendRouted(socket, payload);
}

async function sendServerReply(received) {
  try {
    let reply = received.send();
    for (const part of received.parts) {
      reply = reply.message(part);
    }
    await reply.submit();
    return true;
  } catch (error) {
    if (error instanceof zlink.SubmitError
        && (error.result === zlink.SubmitResult.NotConnected
          || error.result === zlink.SubmitResult.NotFound)) {
      return true;
    }
    throw error;
  }
}

async function runRoutedSendSendRounds({
  sockets,
  payloads,
  measurementRecords,
  routerClient,
  msgSize,
  runId,
  activeStopNs,
  sendDrainStopNs,
  submit = sendPayload,
  drainReplies = async () => {},
  yieldTurn = sleepImmediate,
  nowNs = currentEpochNs
}) {
  let seq = 1n;
  let nextSocket = 0;
  let pendingCount = 0;
  let failure = null;
  const available = sockets.map(() => true);

  const submitOne = (index) => {
    available[index] = false;
    pendingCount += 1;
    let admission;
    try {
      admission = submit(
        sockets[index], routerClient, measurementRecords[index]
      );
    } catch (error) {
      available[index] = true;
      pendingCount -= 1;
      failure ??= error;
      return;
    }

    Promise.resolve(admission).then(
      () => {
        available[index] = true;
        pendingCount -= 1;
      },
      (error) => {
        available[index] = true;
        pendingCount -= 1;
        failure ??= error;
      }
    );
  };

  while (!failure && nowNs() < activeStopNs) {
    const sendStart = nextSocket;
    for (let offset = 0; offset < sockets.length; offset += 1) {
      if (nowNs() >= activeStopNs || failure) break;
      const index = (sendStart + offset) % sockets.length;
      if (!available[index]) continue;

      const currentSeq = seq;
      seq += 1n;
      stampPayload(payloads[index], {
        phase: 1, runId, msgSize, seq: currentSeq
      });
      // A socket owns one stable record until its own public admission
      // settles. A backpressured retry does not gate another socket's submit.
      submitOne(index);
    }
    if (sockets.length > 0) {
      nextSocket = (sendStart + 1) % sockets.length;
    }

    // Receive progress is independent of any one admission Promise. It also
    // releases HWM credit for binding-owned WRITABLE retries.
    await drainReplies();
    await yieldTurn();
  }

  // The active deadline stops new records. Keep receive/retry progress alive
  // for sends whose packets remain owned by the binding until admission.
  while (pendingCount > 0 && nowNs() < sendDrainStopNs) {
    await drainReplies();
    await yieldTurn();
  }

  if (failure) throw failure;
  if (pendingCount > 0) {
    throw new Error('multi routed send admission drain timed out');
  }

  return { sent: seq - 1n };
}

function trackPendingReplyTask(pendingTasks, task, reportFailure) {
  pendingTasks.add(task);
  task.catch(reportFailure)
    .finally(() => pendingTasks.delete(task));
}

async function runRoutedSendSendClient({ options, pattern, routerClient }) {
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'client', pattern);
  const sockets = [];
  const payloads = [];
  const measurementRecords = [];
  const replies = [];
  const poller = zlink.createPoller();
  const pollBuffer = zlink.createPollEvents(Math.max(1, options.clients));
  let rl = null;

  try {
    for (let i = 0; i < options.clients; i += 1) {
      const socket = createClientSocket(ctx, routerClient);
      applySocketPolicy(socket, { transport: options.transport });
      configureTlsClient(socket, options.transport);
      if (routerClient) {
        socket.setRoutingId(
          zlink.RoutingId.from(Buffer.from(`multi-router-client-${i}`, 'ascii'))
        );
        socket.options.setConnectRoutingId(SERVER_ROUTING_ID);
      } else {
        socket.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${i}`, 'ascii')));
      }
      sockets.push(socket);
      const payload = createPayload(options.msgSize);
      payloads.push(payload);
      measurementRecords.push(measurementParts(payload));
      replies.push(new zlink.Received());
    }

    for (let i = 0; i < sockets.length; i += 1) {
      await waitForConnectionReady(sockets[i], () => sockets[i].connect(options.endpoint));
      poller.add(sockets[i], pollEvents(POLLIN), i);
    }
    ctx.recalculateAutoHwm();
    for (const socket of sockets) {
      emitMultiSocketHwmDetail(socket, 'endpoint', options.transport, options.msgSize);
    }

    console.log(`CLIENT_READY,${options.msgSize}`);
    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
      if (line === `START,${options.msgSize}`) {
        break;
      }
      if (line === 'STOP' || line === 'QUIT') {
        return;
      }
    }

    const runId = createRunId(1);
    const activeStartNs = currentEpochNs();
    const activeStopNs = activeStartNs
      + BigInt(Math.floor(options.duration * 1_000_000_000));
    const collector = createMetricCollector({
      suite: 'multi',
      runId,
      msgSize: options.msgSize,
      activeStartNs,
      activeStopNs,
      roundTrip: true
    });
    let repliesSinceYield = 0;
    const drainReadyReplies = async () => {
      const readyCount = poller.wait(pollBuffer, 0);
      for (let offset = 0; offset < readyCount; offset += 1) {
        const index = pollBuffer.slot(offset);
        if (!Number.isInteger(index) || index < 0 || index >= sockets.length) {
          continue;
        }
        const event = { revents: pollBuffer.revents(offset) };
        if (pollEventHas(event, POLLIN)) {
          while (recvNoWaitInto(sockets[index], replies[index])) {
            const reply = replies[index];
            const payload = measurementPayload(reply.parts);
            if (!payload) throw new Error('invalid multipart echo reply');
            collector.recordPayload(payload.data(), currentEpochNs());
            repliesSinceYield += 1;
            if (repliesSinceYield === ASYNC_PROGRESS_BATCH) {
              repliesSinceYield = 0;
              // Refill closes the previous owned parts. Keep the stable
              // per-socket Received wrapper and yield once per bounded batch.
              await sleepImmediate();
            }
          }
        }
      }
    };
    const sendDrainMs = Math.max(1,
      Number(process.env.PERF_MULTI_SEND_DRAIN_TIMEOUT_MS ?? 1000));
    const sendDrainStopNs = activeStopNs + BigInt(Math.floor(sendDrainMs * 1_000_000));

    await runRoutedSendSendRounds({
      sockets,
      payloads,
      measurementRecords,
      routerClient,
      msgSize: options.msgSize,
      runId,
      activeStopNs,
      sendDrainStopNs,
      drainReplies: drainReadyReplies
    });

    await drainReadyReplies();

    const result = await collector.finish();
    for (const metricLine of summarizeMetrics(
      pattern,
      options.transport,
      options.msgSize,
      result.latenciesNs,
      options.duration,
      'current',
      result.accepted,
      result.latencyMeanNs
    )) {
      console.log(metricLine);
    }
    console.log(`CLIENT_DONE,${options.msgSize}`);
  } finally {
    rl?.close();
    pollBuffer.close();
    poller.close();
    for (const reply of replies) {
      reply.close();
    }
    for (const socket of sockets) {
      socket.close();
    }
    ctx.close();
  }
}

async function runRoutedSendSendServer({ options, pattern, family }) {
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'server', pattern);
  const router = zlink.createRouterSocket(ctx);
  const poller = zlink.createPoller();
  const received = new zlink.Received();
  const pendingTasks = new Set();
  let sendFailure = null;
  let replyBatchCount = 0;
  let pollBuffer = null;
  let rl = null;

  try {
    applySocketPolicy(router, { transport: options.transport });
    configureTlsServer(router, options.transport);
    if (family === 'ROUTER_ROUTER') {
      router.setRoutingId(SERVER_ROUTING_ID);
    }
    router.bind(options.endpoint);
    ctx.recalculateAutoHwm();
    emitMultiSocketHwmDetail(router, 'endpoint', options.transport, options.msgSize);
    poller.add(router, pollEvents(POLLIN), 0);
    pollBuffer = zlink.createPollEvents(1);
    const readyBarrier = waitForConnectionReadyCount(router, options.clients);
    console.log(`READY,${options.endpoint}`);

    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    let stopRequested = false;
    const started = await new Promise((resolve) => {
      rl.on('line', (line) => {
        if (line === 'STOP' || line === 'QUIT') {
          stopRequested = true;
          resolve(false);
        } else if (line === `START,${options.msgSize}`) {
          resolve(true);
        }
      });
    });
    if (!started) {
      return;
    }

    await readyBarrier;
    while (!stopRequested) {
      // Pending public send Promises and stdin both run on this event loop.
      // A zero-time readiness probe followed by setImmediate keeps those
      // signal-driven continuations runnable without a timer pump.
      const ready = waitPollerOne(poller, pollBuffer, 0);
      if (ready && pollEventHas(ready, POLLIN)) {
        while (router.recv(received, zlink.RecvFlags.DontWait)) {
          if (!received.routingId) {
            throw new Error('routed echo received without routing id');
          }
          const expectedParts = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
          if (received.parts.length !== expectedParts
              || (expectedParts === 2 && received.parts[1].data().length !== 0)) {
            const partSizes = received.parts.map((part) => part.data().length).join(',');
            throw new Error(
              `invalid multipart echo request: expected=${expectedParts}, sizes=${partSizes}`
            );
          }
          // Received.send captures this source route. Direct admission or a
          // backpressure snapshot consumes the application parts immediately;
          // a pending retry owns only the immutable packet. A terminal failure
          // before either point can leave parts for refill or final close.
          const task = sendServerReply(received);
          trackPendingReplyTask(
            pendingTasks,
            task,
            (error) => { sendFailure ??= error; }
          );
          replyBatchCount += 1;
          if (replyBatchCount === ASYNC_PROGRESS_BATCH) {
            replyBatchCount = 0;
            // Let Promise continuations reap settled tasks without imposing an
            // application reply window; the binding owns WRITABLE retries.
            await sleepImmediate();
            if (sendFailure) throw sendFailure;
          }
        }
      }
      await sleepImmediate();
      if (sendFailure) throw sendFailure;
    }
    await Promise.all(pendingTasks);
    if (sendFailure) throw sendFailure;
  } finally {
    rl?.close();
    received.close();
    pollBuffer?.close();
    poller.close();
    router.close();
    ctx.close();
  }
}

module.exports = {
  resolveRoutedPattern,
  runRoutedSendSendRounds,
  runRoutedSendSendClient,
  runRoutedSendSendServer,
  trackPendingReplyTask
};
