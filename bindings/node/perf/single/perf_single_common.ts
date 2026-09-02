// SPDX-License-Identifier: MPL-2.0

'use strict';

import type { Worker as NodeWorker } from 'node:worker_threads';

const path = require('node:path');
const { Worker } = require('node:worker_threads');
const zlink = require('@zlink-systems/zlink');
const {
  MonitorEventType,
  RecvFlags,
  RecvResult
} = zlink;
const {
  createMetricCollector,
  createPayload,
  createRunId,
  currentEpochNs,
  HEADER_SIZE,
  MIN_MSG_SIZE,
  applyAutoHwmProfile,
  integerEnv,
  manualSocketOverridesEnabled,
  stampPayload
} = require('../common/perf_metrics');
const {
  configureTlsClient,
  configureTlsServer,
} = require('../common/perf_tls');
const { isStopTokenParts } = require('../perf_stop_token');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
const { benchmarkEndpoint: commonBenchmarkEndpoint } = require('../common/perf_endpoint');
const POLLIN = 1;

function measurementPartCount() {
  return process.env.PERF_PART_COUNT === '1' ? 1 : 2;
}

function appendMeasurement(op, payload) {
  op = op.message(payload);
  if (measurementPartCount() === 2) {
    op = op.message(Buffer.alloc(0));
  }
  return op;
}

function measurementPayload(parts) {
  if (!Array.isArray(parts) || parts.length !== measurementPartCount()) {
    return null;
  }
  if (measurementPartCount() === 2 && parts[1].data().length !== 0) {
    return null;
  }
  return parts[0];
}

function pollEvents(mask) {
  const events = [];
  if ((mask & POLLIN) !== 0) {
    events.push(zlink.PollEventFlag.PollIn);
  }
  return events;
}

async function benchmarkEndpoint(transport, token) {
  return commonBenchmarkEndpoint(transport, token, { suite: 'single' });
}

interface SingleSocketPolicyOptions {
  transport?: string;
  hwm?: number;
  sendHwm?: number;
  recvHwm?: number;
  sendTimeoutMs?: number;
  recvTimeoutMs?: number;
  recvTimeout?: number;
  lingerMs?: number;
  noDrop?: boolean;
  policySocketOverrides?: boolean;
}

interface RecordUntilOptions {
  recordUntilNs?: bigint | number | string;
}

type SenderWorker = NodeWorker;

interface SenderWorkerState {
  control: Int32Array;
  status: Int32Array;
}

const senderWorkerStates = new WeakMap<SenderWorker, SenderWorkerState>();
const sleepState = new Int32Array(new SharedArrayBuffer(4));

function sleepMillisSync(ms) {
  Atomics.wait(sleepState, 0, 0, Math.max(0, ms | 0));
}

function senderWorkerState(worker: SenderWorker): SenderWorkerState {
  const state = senderWorkerStates.get(worker);
  if (!state) {
    throw new Error('sender worker is not managed by perf_single_common');
  }
  return state;
}

function applySocketPolicy(socket, options: SingleSocketPolicyOptions = {}) {
  const manualOverrides =
    manualSocketOverridesEnabled('single');
  const policyOverrides = options.policySocketOverrides === true;
  const hwm = Number.isFinite(options.hwm)
    ? options.hwm
    : integerEnv('PERF_SINGLE_HWM', NaN);
  const sendHwm = Number.isFinite(options.sendHwm)
    ? options.sendHwm
    : integerEnv('PERF_SINGLE_SNDHWM', hwm);
  const recvHwm = Number.isFinite(options.recvHwm)
    ? options.recvHwm
    : integerEnv('PERF_SINGLE_RCVHWM', hwm);
  const sendTimeout = Number.isFinite(options.sendTimeoutMs)
    ? options.sendTimeoutMs
    : integerEnv('PERF_SINGLE_SNDTIMEO_MS', 200);
  const recvTimeout = Number.isFinite(options.recvTimeoutMs)
    ? options.recvTimeoutMs
    : integerEnv('PERF_SINGLE_RCVTIMEO_MS', 200);
  const linger = Number.isFinite(options.lingerMs)
    ? options.lingerMs
    : integerEnv('PERF_SINGLE_LINGER_MS', 0);

  if (socket.options) {
    if (manualOverrides || policyOverrides) {
      if (Number.isFinite(sendHwm) && sendHwm > 0) {
        socket.options.sendHwm = BigInt(sendHwm);
      }
      if (Number.isFinite(recvHwm) && recvHwm > 0) {
        socket.options.recvHwm = BigInt(recvHwm);
      }
    }
    socket.options.sendTimeout = sendTimeout;
    socket.options.recvTimeout = options.recvTimeout ?? recvTimeout;
    socket.options.linger = linger;
    if ('noDrop' in socket.options && options.noDrop !== undefined) {
      socket.options.noDrop = Boolean(options.noDrop);
    }
  }
}

function socketTypeName(socket) {
  if (typeof socket.recvPacket === 'function') return 'stream';
  if (typeof socket.reply === 'function') return 'router';
  if (typeof socket.request === 'function') return 'dealer';
  if (typeof socket.publish === 'function') return 'pub';
  if (typeof socket.subscribe === 'function') return 'sub';
  if (typeof socket.send === 'function' && typeof socket.recv === 'function') return 'pair';
  return 'unknown';
}

function autoHwmRoleName(role) {
  switch (role) {
    case 1: return 'control';
    case 2: return 'routed';
    case 3: return 'fanout';
    case 4: return 'recv_ingress';
    case 6: return 'peer_queue';
    case 7: return 'stream';
    default: return 'none';
  }
}

function singleAutoHwmSnapshotVisible(snapshot) {
  return snapshot.autoHwmAppliedSndHwmBytes > 0n
    || snapshot.autoHwmAppliedRcvHwmBytes > 0n
    || BigInt(snapshot.sndPendingBytes ?? 0) > 0n
    || BigInt(snapshot.rcvPendingBytes ?? 0) > 0n;
}

function emitSingleSocketHwmDetail(socket, pattern, transport, component, msgSize) {
  if (!socket || !pattern || !component) {
    return;
  }
  // Objects without a monitor surface do not emit this optional diagnostic.
  if (typeof socket.monitorOpen !== 'function') {
    return;
  }
  // PERF_POLICY § 1.1.4: do not silently swallow real failures. A failed
  // `monitorOpen` on a socket that DOES support it means a broken/closed
  // socket — a real benchmark fault that must surface. Only the snapshot
  // read + diagnostic emission itself is best-effort (it never affects
  // the measured RESULT).
  const monitor = socket.monitorOpen([MonitorEventType.ConnectionReady]);
  try {
    const snapshot = monitor.status();
    if (!singleAutoHwmSnapshotVisible(snapshot)) {
      return;
    }
    console.log(
      'AUTO_HWM_DETAIL'
      + `,pattern=${pattern}`
      + `,transport=${transport}`
      + `,component=${component}`
      + `,msg_size=${msgSize}`
      + ',owner=socket'
      + ',owner_id=0'
      + `,socket=${component}`
      + `,socket_type=${socketTypeName(socket)}`
      + `,role=${autoHwmRoleName(snapshot.autoHwmRole)}`
      + `,sndhwm=${snapshot.autoHwmAppliedSndHwmBytes}`
      + `,rcvhwm=${snapshot.autoHwmAppliedRcvHwmBytes}`
      + `,snd_pending_bytes=${snapshot.sndPendingBytes}`
      + `,rcv_pending_bytes=${snapshot.rcvPendingBytes}`
      + `,effective_sndbuf=${snapshot.autoHwmEffectiveSndBuf}`
      + `,effective_rcvbuf=${snapshot.autoHwmEffectiveRcvBuf}`
    );
  } catch (err) {
    // Snapshot/emit is diagnostic only; keep the benchmark result primary.
  } finally {
    monitor?.close();
  }
}

function applyContextPolicy(ctx) {
  const ioThreads = integerEnv('PERF_IO_THREADS', 0);
  if (ioThreads > 0) {
    ctx.options.ioThreads = ioThreads;
  }
  const maxSockets = integerEnv('PERF_MAX_SOCKETS', NaN);
  if (Number.isFinite(maxSockets) && maxSockets > 0) {
    ctx.options.maxSockets = maxSockets;
  }
  if ('autoHwmEnabled' in ctx.options) {
    ctx.options.autoHwmEnabled = integerEnv('PERF_CTX_AUTO_HWM_ENABLE', 1) !== 0;
  }
  applyAutoHwmProfile(ctx, zlink);
}

function recvNoWait(socket, received = new zlink.Received(), flags = RecvFlags.DontWait) {
  try {
    return socket.recv(received, flags) ? received : null;
  } catch (error) {
    if (error instanceof zlink.RecvError && error.result === RecvResult.NoData) {
      return null;
    }
    throw error;
  }
}

function subscribeNoWait(socket, received = new zlink.TopicMessage(), flags = RecvFlags.DontWait) {
  try {
    return socket.subscribe(received, flags) ? received : null;
  } catch (error) {
    if (error instanceof zlink.RecvError && error.result === RecvResult.NoData) {
      return null;
    }
    throw error;
  }
}

function isStopTokenPayload(buffer, size) {
  if (size !== STOP_TOKEN_BYTES.length) {
    return false;
  }
  for (let i = 0; i < STOP_TOKEN_BYTES.length; i += 1) {
    if (buffer[i] !== STOP_TOKEN_BYTES[i]) {
      return false;
    }
  }
  return true;
}

function waitForConnectionReady(
  socket,
  connectFn = null,
  timeoutMs = integerEnv('PERF_CONNECT_READY_TIMEOUT_MS', 1000)
) {
  const monitor = socket.monitorOpen([MonitorEventType.ConnectionReady]);
  try {
    if (typeof connectFn === 'function') {
      connectFn();
    }
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      try {
        const event = monitor.recv(RecvFlags.DontWait);
        if (event && event.event === MonitorEventType.ConnectionReady) {
          return;
        }
      } catch (error) {
        if (!(error instanceof zlink.RecvError && error.result === RecvResult.NoData)) {
          throw error;
        }
      }
      sleepMillisSync(1);
    }
    throw new Error(`connection ready timeout after ${timeoutMs}ms`);
  } finally {
    monitor.close();
  }
}

function waitForMonitorConnectionReady(
  monitor,
  timeoutMs = integerEnv('PERF_CONNECT_READY_TIMEOUT_MS', 1000)
) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const event = monitor.recv(RecvFlags.DontWait);
      if (event && event.event === MonitorEventType.ConnectionReady) {
        return;
      }
    } catch (error) {
      if (!(error instanceof zlink.RecvError && error.result === RecvResult.NoData)) {
        throw error;
      }
    }
    sleepMillisSync(1);
  }
  throw new Error(`connection ready timeout after ${timeoutMs}ms`);
}

function waitForPostReadySettle(timeoutMs) {
  sleepMillisSync(Math.max(0, timeoutMs | 0));
}

// C parity: bindings/c/perf/single/common/perf_single_one_way.hpp
// run_active_phase (~269-326) — the receiver thread issues a BLOCKING recv
// bounded by the socket-level `ZLINK_OPT_RCVTIMEO` (set by
// bench_common_runtime.hpp:516 to `PERF_SINGLE_RCVTIMEO_MS`, default 200),
// so recv returns EAGAIN on idle and the loop keeps CYCLING; it only EXITS
// on the wire stop token or a real error (phase end stays wire-stop-token
// driven, PERF_SINGLE_TEST_POLICY § 1.4). C is the sole authority: although
// PERF_SINGLE_TEST_POLICY.md:131 lists the receiver poller wait as `-1`,
// the C reference bounds every individual recv by RCVTIMEO and cycles, so
// we match C and bound the poller wait by the same RCVTIMEO budget rather
// than blocking on `-1`.
//
// This is also load-bearing for correctness: `Poller.wait(events, timeout)` is a
// synchronous N-API call that blocks the JS event loop. An unbounded
// (`-1`) wait blocks the receiver — and the JS loop — forever when the
// peer Worker stalls (observed: a hung DEALER_DEALER/ipc case with the
// main thread parked in `do_sys_poll` and the worker thread futex-blocked
// inside a native zlink call). The `Promise.race` against the sender-Worker
// error channel can then never settle, hard-hanging the case to the
// harness timeout. The RCVTIMEO-bounded wakeup mirrors C's EAGAIN cycle and
// keeps every synchronous blocking receive bounded.
function drainRecvSocket(socket, onMessage, options: RecordUntilOptions = {}) {
  const useSubscribe = typeof socket.subscribe === 'function';
  const recvTimeoutMs = Math.max(
    1,
    integerEnv('PERF_SINGLE_RCVTIMEO_MS', 200)
  );
  const recordUntilNs = options.recordUntilNs === undefined
    ? null
    : BigInt(options.recordUntilNs);

  let stopReceived = false;
  let iterCount = 0;
  let totalReceived = 0;
  let recordingActive = true;
  const reusableReceived = useSubscribe
    ? new zlink.TopicMessage()
    : new zlink.Received();
  if (process.env.PERF_NODE_TRACE === '1') {
    console.error(`[drainRecvSocket] entry`);
  }
  while (!stopReceived) {
    iterCount += 1;
    if (process.env.PERF_NODE_TRACE === '1' && (iterCount === 1 || iterCount % 100 === 0)) {
      console.error(`[drainRecvSocket] iter=${iterCount} totalReceived=${totalReceived}`);
    }
    let first = true;
    while (true) {
      const received = useSubscribe
        ? subscribeNoWait(socket, reusableReceived, first ? RecvFlags.None : RecvFlags.DontWait)
        : recvNoWait(socket, reusableReceived, first ? RecvFlags.None : RecvFlags.DontWait);
      if (!received) {
        break;
      }
      first = false;
      if (isStopTokenParts(received.parts)) {
        stopReceived = true;
        if (process.env.PERF_NODE_TRACE === '1') {
          console.error(`[drainRecvSocket] stop totalReceived=${totalReceived}`);
        }
        break;
      }
      totalReceived += 1;
      if (process.env.PERF_NODE_TRACE === '1' && (totalReceived % 100000) === 0) {
        console.error(`[drainRecvSocket] received=${totalReceived}`);
      }
      if (recordUntilNs !== null && recordingActive) {
        recordingActive = currentEpochNs() <= recordUntilNs;
      }
      if (recordUntilNs !== null && !recordingActive) {
        continue;
      }
      onMessage(received);
    }
  }
}

function drainRouterRecvInto(router, msgSize, onHeader, options: RecordUntilOptions = {}) {
  const payloadSize = Math.max(msgSize, HEADER_SIZE);
  const recordUntilNs = options.recordUntilNs === undefined
    ? null
    : BigInt(options.recordUntilNs);
  const metricCollector = typeof onHeader?.recordLatencyNs === 'function'
    ? onHeader
    : null;
  let stopReceived = false;
  let iterCount = 0;
  let totalReceived = 0;
  let recordingActive = true;
  const received = new zlink.Received();
  if (process.env.PERF_NODE_TRACE === '1') {
    console.error(`[drainRouterRecvInto] entry`);
  }
  while (!stopReceived) {
    iterCount += 1;
    if (process.env.PERF_NODE_TRACE === '1' && (iterCount === 1 || iterCount % 100 === 0)) {
      console.error(`[drainRouterRecvInto] iter=${iterCount} totalReceived=${totalReceived}`);
    }
    let first = true;
    while (true) {
      if (!recvNoWait(router, received, first ? RecvFlags.None : RecvFlags.DontWait)) {
        break;
      }
      first = false;
      if (isStopTokenParts(received.parts)) {
        stopReceived = true;
        if (process.env.PERF_NODE_TRACE === '1') {
          console.error(`[drainRouterRecvInto] stop totalReceived=${totalReceived}`);
        }
        break;
      }
      const payload = measurementPayload(received.parts);
      const receivedAtNs = currentEpochNs();
      if (!payload) {
        if (metricCollector !== null) {
          metricCollector.recordPayload(null, receivedAtNs);
        } else {
          onHeader(null, receivedAtNs);
        }
        continue;
      }
      const data = payload.data();
      const receivedSize = data.length;
      totalReceived += 1;
      if (process.env.PERF_NODE_TRACE === '1' && (totalReceived % 100000) === 0) {
        console.error(`[drainRouterRecvInto] received=${totalReceived}`);
      }
      if (recordUntilNs !== null && recordingActive) {
        recordingActive = receivedAtNs <= recordUntilNs;
      }
      if (recordUntilNs !== null && !recordingActive) {
        continue;
      }
      if (metricCollector !== null) {
        metricCollector.recordPayload(data, receivedAtNs);
        continue;
      }
      if (receivedSize !== payloadSize) {
        onHeader(null, receivedAtNs);
        continue;
      }
      onHeader(data, receivedAtNs);
    }
  }
}

function isTransientSubmit(error) {
  const text = String(error && error.message ? error.message : error);
  return (error instanceof zlink.SubmitError
      && (error.result === zlink.SubmitResult.Backpressured
        || error.result === zlink.SubmitResult.NotConnected
        || error.result === zlink.SubmitResult.NotFound))
    || (error && error.code === 'EAGAIN')
    || text.includes('Resource temporarily unavailable')
    || text.includes('Host unreachable')
    || text.includes('Transport endpoint is not connected');
}

function sendSocketNoWait(socket, payload) {
  try {
    appendMeasurement(socket.send(), payload).submit_sync();
    return true;
  } catch (error) {
    if (isTransientSubmit(error)) {
      return false;
    }
    throw error;
  }
}

function sendSocketRequired(socket, payload) {
  appendMeasurement(socket.send(), payload).submit_sync();
}

function sendSocketStopWithRetry(socket) {
  for (let retry = 0; retry < 100; retry += 1) {
    try {
      socket.send().message(STOP_TOKEN_BYTES).submit_sync();
      return;
    } catch (error) {
      if (!isTransientSubmit(error)) {
        throw error;
      }
    }
  }
  throw new Error('stop token send retry budget exhausted');
}

function drainRecvSocketNoWaitUntilIdle(socket, collector, payloadSize, viaSubscribe = false) {
  let stopReceived = false;
  while (true) {
    const received = viaSubscribe ? subscribeNoWait(socket) : recvNoWait(socket);
    if (!received) {
      return stopReceived;
    }
    if (isStopTokenParts(received.parts)) {
      stopReceived = true;
      continue;
    }
    const payload = measurementPayload(received.parts);
    if (!payload) {
      collector.recordPayload(null, currentEpochNs());
      continue;
    }
    const data = payload.data();
    if (data.length !== payloadSize) {
      collector.recordPayload(null, currentEpochNs());
      continue;
    }
    collector.recordPayload(data, currentEpochNs());
  }
}

function runLocalSocketOneWayBenchmark({ pattern }) {
  // inproc is context-local, while Node Workers cannot share a Context.
  // A single JavaScript loop that alternates send and recv imposes a hidden
  // window and does not represent C's concurrent sender/receiver workload.
  return { unsupported: true, pattern };
}

function parseSingleBinaryArgs(argv) {
  if (argv.length < 3) {
    throw new Error('usage: <binary> <lib_name> <transport> <size>');
  }
  const transport = String(argv[1] || '').trim().toLowerCase();
  const msgSize = Number(argv[2]);
  if (!Number.isFinite(msgSize) || msgSize < MIN_MSG_SIZE) {
    throw new Error(`invalid single msg size: ${argv[2]}`);
  }
  return {
    libName: String(argv[0] || 'current'),
    transport,
    msgSize,
    duration: integerEnv('PERF_SINGLE_DURATION_SECONDS', 5),
    runId: 1,
    hwm: integerEnv('PERF_SINGLE_HWM', NaN),
    sendHwm: integerEnv('PERF_SINGLE_SNDHWM', NaN),
    recvHwm: integerEnv('PERF_SINGLE_RCVHWM', NaN),
    sendTimeoutMs: integerEnv('PERF_SINGLE_SNDTIMEO_MS', NaN),
    recvTimeoutMs: integerEnv('PERF_SINGLE_RCVTIMEO_MS', NaN)
  };
}

function spawnSenderWorker(workerData): SenderWorker {
  const controlBuffer = new SharedArrayBuffer(4);
  const control = new Int32Array(controlBuffer);
  const statusBuffer = new SharedArrayBuffer(4);
  const status = new Int32Array(statusBuffer);
  const worker = new Worker(
    path.join(__dirname, 'perf_single_sender_worker.js'),
    { workerData: { ...workerData, controlBuffer, statusBuffer } }
  ) as SenderWorker;
  senderWorkerStates.set(worker, { control, status });
  return worker;
}

function waitForWorkerStatus(
  worker: SenderWorker,
  expectedStatus: number,
  timeoutMs = integerEnv('PERF_CONNECT_READY_TIMEOUT_MS', 1000)
) {
  const status = senderWorkerState(worker).status;
  const deadline = Date.now() + Math.max(1, timeoutMs | 0);
  for (;;) {
    const current = Atomics.load(status, 0);
    if (current < 0) {
      throw new Error('sender worker failed during synchronous benchmark setup');
    }
    if (current >= expectedStatus) {
      return;
    }
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      throw new Error(`worker status timeout waiting for ${expectedStatus}`);
    }
    Atomics.wait(status, 0, current, remaining);
  }
}

function releaseSenderWorker(worker: SenderWorker) {
  const control = senderWorkerState(worker).control;
  Atomics.store(control, 0, 1);
  Atomics.notify(control, 0);
}

async function closeSenderWorker(worker?: NodeWorker | null) {
  if (!worker) {
    return;
  }
  const waitForExit = new Promise<void>((resolve) => {
    worker.once('exit', () => resolve());
  });
  try {
    const control = senderWorkerState(worker).control;
    Atomics.store(control, 0, 2);
    Atomics.notify(control, 0);
  } catch (err) {
    console.error(`[perf] close failed: ${err}`);
  }
  const exited = await Promise.race([
    waitForExit.then(() => true),
    new Promise((resolve) => setTimeout(() => resolve(false), 1000))
  ]);
  if (!exited && worker.threadId && Number.isFinite(worker.threadId)) {
    try {
      await worker.terminate();
    } catch (err) {
      console.error(`[perf] close failed: ${err}`);
    }
  }
}

module.exports = {
  applyContextPolicy,
  applySocketPolicy,
  configureTlsClient,
  configureTlsServer,
  emitSingleSocketHwmDetail,
  benchmarkEndpoint,
  appendMeasurement,
  closeSenderWorker,
  drainRouterRecvInto,
  drainRecvSocket,
  parseSingleBinaryArgs,
  measurementPayload,
  runLocalSocketOneWayBenchmark,
  releaseSenderWorker,
  sendSocketRequired,
  spawnSenderWorker,
  waitForWorkerStatus,
  waitForPostReadySettle,
  waitForConnectionReady,
  waitForMonitorConnectionReady,
};

export {
  applyContextPolicy,
  applySocketPolicy,
  configureTlsClient,
  configureTlsServer,
  emitSingleSocketHwmDetail,
  benchmarkEndpoint,
  appendMeasurement,
  closeSenderWorker,
  drainRouterRecvInto,
  drainRecvSocket,
  parseSingleBinaryArgs,
  measurementPayload,
  runLocalSocketOneWayBenchmark,
  releaseSenderWorker,
  sendSocketRequired,
  spawnSenderWorker,
  waitForWorkerStatus,
  waitForPostReadySettle,
  waitForConnectionReady,
  waitForMonitorConnectionReady,
};
