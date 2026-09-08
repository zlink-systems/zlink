// SPDX-License-Identifier: MPL-2.0

'use strict';

const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createPayload,
  createRunId,
  decodeMetricHeader,
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
  POLLCOMPLETION,
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

// Read once per process: the runner fixes PERF_PART_COUNT before launching
// this process, and this is on the per-message recv path. A per-message
// `process.env` lookup puts harness instrumentation inside the measured path.
const MEASUREMENT_PART_COUNT = process.env.PERF_PART_COUNT === '1' ? 1 : 2;

const SERVER_ROUTING_ID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
const ASYNC_PROGRESS_BATCH = 64;
const RELAY_TEARDOWN_RESERVE_MS = 2000;

function positiveEnvMs(names, fallback) {
  for (const name of names) {
    const parsed = Number(process.env[name]);
    if (Number.isFinite(parsed) && parsed > 0) return Math.floor(parsed);
  }
  return fallback;
}

function relayShutdownDrainMs() {
  const sendDrainMs = positiveEnvMs(['PERF_MULTI_SEND_DRAIN_TIMEOUT_MS'], 5000);
  const shutdownMs = positiveEnvMs([
    'PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS',
    'PERF_SERVER_SHUTDOWN_TIMEOUT_MS'
  ], 5000);
  return Math.max(250, Math.min(sendDrainMs, shutdownMs - RELAY_TEARDOWN_RESERVE_MS));
}

// Echo clients keep their sockets open until every admitted record has been
// received. This count is observed only during teardown; it never gates sends
// in the active window.
class EchoReplyDrain {
  pending: number;

  constructor() {
    this.pending = 0;
  }

  // Count before awaiting admission because receive dispatch can precede the
  // Promise continuation even though Core has already admitted the record.
  submitted() {
    this.pending += 1;
  }

  admissionRejected() {
    this.finishOne('echo admission rejection without a matching submission');
  }

  received() {
    this.finishOne('echo reply without a matching submission');
  }

  finishOne(errorMessage) {
    if (this.pending === 0) throw new Error(errorMessage);
    this.pending -= 1;
  }
}

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

async function sendServerReply(router, routingId, parts) {
  try {
    await sendRouted(router, routingId, parts);
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

interface PendingRoutedReply {
  routingId: any;
  parts: Buffer[];
  next: PendingRoutedReply | null;
}

class RoutedReplySender {
  router: any;
  pendingHead: PendingRoutedReply | null;
  pendingTail: PendingRoutedReply | null;
  task: Promise<void> | null;
  failure: unknown;
  pendingCount: number;
  submitReply: (router: any, routingId: any, parts: Buffer[]) => Promise<boolean>;

  constructor(router, submitReply = sendServerReply) {
    this.router = router;
    this.pendingHead = null;
    this.pendingTail = null;
    this.task = null;
    this.failure = null;
    this.pendingCount = 0;
    this.submitReply = submitReply;
  }

  enqueue(received) {
    this.raiseIfFailed();
    const reply = {
      routingId: zlink.RoutingId.from(received.routingId.toBytes()),
      parts: received.parts.map((part) => Buffer.from(part.data())),
      next: null
    };
    if (this.pendingTail) {
      this.pendingTail.next = reply;
    } else {
      this.pendingHead = reply;
    }
    this.pendingTail = reply;
    this.pendingCount += 1;
    this.start();
  }

  raiseIfFailed() {
    if (this.failure) throw this.failure;
  }

  async drain() {
    while (this.task) {
      await this.task;
    }
    this.raiseIfFailed();
  }

  async drainUntil(deadlineNs, nowNs = currentEpochNs, yieldTurn = sleepImmediate) {
    while (this.task && BigInt(nowNs()) < BigInt(deadlineNs)) {
      await yieldTurn();
    }
    this.raiseIfFailed();
    return this.task === null;
  }

  start() {
    if (this.task || this.failure || !this.pendingHead) return;
    const task = this.sendPending();
    this.task = task;
    task.then(
      () => {
        if (this.task !== task) return;
        this.task = null;
        this.start();
      },
      (error) => {
        if (this.task === task) this.failure ??= error;
      }
    );
  }

  async sendPending() {
    while (this.pendingHead) {
      const reply = this.pendingHead;
      await this.submitReply(this.router, reply.routingId, reply.parts);
      this.pendingHead = reply.next;
      if (!this.pendingHead) this.pendingTail = null;
      this.pendingCount -= 1;
    }
  }
}

async function waitForReplyAdmissionOrStop({
  replies,
  stopSignal,
  progress = () => {},
  yieldTurn = sleepImmediate
}) {
  while (replies.pendingCount > 0 && !stopSignal.aborted) {
    progress();
    await yieldTurn();
    replies.raiseIfFailed();
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
  replyDrain = null,
  submit = sendPayload,
  drainReplies = async (_timeoutMs = 0) => {},
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
    replyDrain?.submitted();
    let admission;
    try {
      admission = submit(
        sockets[index], routerClient, measurementRecords[index]
      );
    } catch (error) {
      available[index] = true;
      pendingCount -= 1;
      try {
        replyDrain?.admissionRejected();
      } catch (drainError) {
        failure ??= drainError;
        return;
      }
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
        try {
          replyDrain?.admissionRejected();
        } catch (drainError) {
          failure ??= drainError;
          return;
        }
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
  // inside the existing teardown deadline. Once admissions have settled, the
  // poller can wait for echoes without delaying a Promise continuation.
  while (pendingCount > 0 || (replyDrain?.pending ?? 0) > 0) {
    const remainingNs = BigInt(sendDrainStopNs) - BigInt(nowNs());
    if (remainingNs <= 0n) break;
    const waitMs = pendingCount === 0
      ? Math.max(1, Number(remainingNs / 1_000_000n))
      : 0;
    await drainReplies(waitMs);
    await yieldTurn();
  }

  if (failure) throw failure;
  if (pendingCount > 0 || (replyDrain?.pending ?? 0) > 0) {
    throw new Error(
      'multi routed send admission or echo drain timed out '
      + `(echoes=${replyDrain?.pending ?? 0}, admissions=${pendingCount})`
    );
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
      poller.add(sockets[i], pollEvents(POLLIN | POLLCOMPLETION), i);
    }
    ctx.recalculateAutoHwm();
    for (const socket of sockets) {
      emitMultiSocketHwmDetail(socket, 'endpoint', options.transport, options.msgSize);
    }

    // PERF_POLICY.md:469-471 - the C send/send echo client uses no runner
    // CLIENT_READY/START barrier (bindings/c/perf/multi/src/
    // perf_multi_dealer_router_client.cpp emits neither token). Its own
    // CONNECTION_READY gate above is the whole ready condition.

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
    const replyDrain = new EchoReplyDrain();
    let repliesSinceYield = 0;
    const drainReadyReplies = async (timeoutMs = 0) => {
      const readyCount = poller.wait(pollBuffer, timeoutMs);
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
            const data = payload.data();
            const header = decodeMetricHeader(data);
            if (!header || data.length !== options.msgSize
                || header.runId !== runId || header.phase !== 1
                || header.msgSize !== options.msgSize) {
              continue;
            }
            const receivedAtNs = currentEpochNs();
            replyDrain.received();
            if (receivedAtNs >= activeStopNs) continue;
            collector.recordPayload(data, receivedAtNs);
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
    // C echo client (perf_multi_client_helpers.hpp): the teardown window is
    // max(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS, 3 s per active second) because
    // small messages can fill every per-client Core queue.
    const sendDrainMs = Math.max(
      Math.max(1, Number(process.env.PERF_MULTI_SEND_DRAIN_TIMEOUT_MS ?? 5000)),
      Math.max(1, Math.floor(options.duration)) * 3000);
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
      replyDrain,
      drainReplies: drainReadyReplies
    });

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
    // No CLIENT_DONE here: the C send/send echo client emits none
    // (PERF_POLICY.md:469-471, D-2). The runner observes client exit.
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
  const replies = new RoutedReplySender(router);
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
    poller.add(router, pollEvents(POLLIN | POLLCOMPLETION), 0);
    pollBuffer = zlink.createPollEvents(1);
    const readyBarrier = waitForConnectionReadyCount(router, options.clients);
    console.log(`READY,${options.endpoint}`);

    // PERF_POLICY.md:469-471 - the C send/send echo relay server has only a
    // stdin STOP/QUIT watcher and no runner START gate
    // (bindings/c/perf/multi/common/perf_multi_relay_server.hpp:667-677).
    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    const stopController = new AbortController();
    rl.on('line', (line) => {
      if (line === 'STOP' || line === 'QUIT') {
        stopController.abort();
      }
    });

    await readyBarrier;
    while (!stopController.signal.aborted) {
      // Pending public send Promises and stdin both run on this event loop.
      // A zero-time readiness probe followed by setImmediate keeps those
      // signal-driven continuations runnable without a timer pump.
      const ready = waitPollerOne(poller, pollBuffer, 0);
      if (ready && pollEventHas(ready, POLLIN)) {
        while (router.recv(received, zlink.RecvFlags.DontWait)) {
          if (!received.routingId) {
            throw new Error('routed echo received without routing id');
          }
          const expectedParts = MEASUREMENT_PART_COUNT;
          if (received.parts.length !== expectedParts
              || (expectedParts === 2 && received.parts[1].data().length !== 0)) {
            const partSizes = received.parts.map((part) => part.data().length).join(',');
            throw new Error(
              `invalid multipart echo request: expected=${expectedParts}, sizes=${partSizes}`
            );
          }
          // Capture one immutable snapshot for the single sender.
          replies.enqueue(received);
          // Preserve Core receive backpressure like the C relay: do not move
          // another request into the application FIFO until this reply is
          // admitted. STOP remains able to interrupt this wait so the existing
          // bounded shutdown drain owns the final pending admission.
          await waitForReplyAdmissionOrStop({
            replies,
            stopSignal: stopController.signal,
            progress: () => { waitPollerOne(poller, pollBuffer, 0); }
          });
          replies.raiseIfFailed();
          if (stopController.signal.aborted) break;
          replyBatchCount += 1;
          if (replyBatchCount === ASYNC_PROGRESS_BATCH) {
            replyBatchCount = 0;
            // This is only a scheduler fairness budget. It neither caps the
            // pending FIFO nor changes its one-at-a-time admission rule.
            await sleepImmediate();
            replies.raiseIfFailed();
          }
        }
      }
      await sleepImmediate();
      replies.raiseIfFailed();
    }
    // Node SEND admission is asynchronous: sendRouted() returns the Promise
    // from op.submit(). STOP therefore does not block the event loop, but an
    // unconditional await here can still wait forever for a departed peer's
    // WRITABLE token. Give the already-owned reply the same bounded post-STOP
    // drain as the C relay, while reserving time for socket/context teardown.
    const drainMs = relayShutdownDrainMs();
    const drainDeadlineNs = currentEpochNs() + BigInt(drainMs) * 1_000_000n;
    if (!(await replies.drainUntil(drainDeadlineNs))) {
      console.error(
        '[perf-multi-relay] shutdown drain expired '
        + `window_ms=${drainMs} pending_replies=${replies.pendingCount}`
      );
      console.error(
        '[perf-multi-relay] reply abandoned after shutdown drain; '
        + 'socket close will terminate its admission Promise'
      );
    }
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
  RoutedReplySender,
  relayShutdownDrainMs,
  trackPendingReplyTask,
  waitForReplyAdmissionOrStop
};
