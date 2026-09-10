// SPDX-License-Identifier: MPL-2.0

'use strict';

const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createPayload,
  createRunId,
  currentEpochNs,
  sleepImmediate,
  stampPayload,
  summarizeMetrics
} = require('../common/perf_metrics');
const { configureTlsClient } = require('../common/perf_tls');
const {
  appendMeasurement,
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  waitForConnectionReady
} = require('./perf_multi_runtime');

// Read once per process: the runner fixes PERF_PART_COUNT before launching
// this process, and this is on the per-message path. A per-message
// `process.env` lookup puts harness instrumentation inside the measured
// path and charges it only to the binding runner. C reference:
// bindings/c/perf/common/perf_zlink_part_helpers.hpp
// perf_measurement_part_count.
const MEASUREMENT_PART_COUNT = process.env.PERF_PART_COUNT === '1' ? 1 : 2;

function measurementPayload(parts) {
  const count = MEASUREMENT_PART_COUNT;
  if (!Array.isArray(parts) || parts.length !== count) return null;
  if (count === 2 && parts[1].data().length !== 0) return null;
  return parts[0];
}

function closeParts(parts) {
  for (const part of parts ?? []) part?.close?.();
}

async function runSocketReqRepClient({ options, pattern, routerClient, serverRoutingId }) {
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'client', pattern);
  const sockets = [];
  let rl = null;

  try {
    for (let i = 0; i < options.clients; i += 1) {
      const socket = routerClient ? zlink.createRouterSocket(ctx) : zlink.createDealerSocket(ctx);
      applySocketPolicy(socket, { transport: options.transport });
      configureTlsClient(socket, options.transport);
      socket.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${i}`, 'ascii')));
      if (routerClient) socket.options.setConnectRoutingId(serverRoutingId);
      sockets.push(socket);
      await waitForConnectionReady(socket, () => socket.connect(options.endpoint));
    }
    ctx.recalculateAutoHwm();
    for (const socket of sockets) {
      emitMultiSocketHwmDetail(socket, 'endpoint', options.transport, options.msgSize);
    }
    // PERF_POLICY.md:469-471 - the C request/reply client uses no runner
    // CLIENT_READY/START barrier; its own CONNECTION_READY gate above is the
    // whole ready condition. A binding runner must not add one.

    const runId = createRunId(1);
    const activeStartNs = currentEpochNs();
    const activeStopNs = activeStartNs
      + BigInt(Math.floor(options.duration * 1_000_000_000));
    const collector = createMetricCollector({
      suite: 'multi', runId, msgSize: options.msgSize,
      activeStartNs, activeStopNs, roundTrip: true
    });
    const requestTimeoutMs = Math.max(1,
      Number(process.env.PERF_MULTI_REQREP_TIMEOUT_MS ?? 200));
    let seq = 1n;

    const payloadTemplates = sockets.map(() => createPayload(options.msgSize));
    const pending = new Set();
    const blocked = new Map();
    const available = sockets.map(() => true);
    let nextSocket = 0;
    let requestFailure = null;

    const collectReply = async (reply) => {
      let parts = null;
      try {
        parts = await reply;
        const replyPayload = measurementPayload(parts);
        collector.recordPayload(replyPayload?.data?.() ?? null, currentEpochNs());
      } catch (error) {
        if (error instanceof zlink.RequestError
            && error.result === zlink.RequestResult.TimedOut) return;
        throw error;
      } finally {
        closeParts(parts);
      }
    };

    while (currentEpochNs() < activeStopNs) {
      const sendStart = nextSocket;
      for (let offset = 0; offset < sockets.length; offset += 1) {
        const index = (sendStart + offset) % sockets.length;
        if (!available[index]) continue;
        const payload = Buffer.from(payloadTemplates[index]);
        const currentSeq = seq;
        seq += 1n;
        // Concurrent logical requests cannot share the stamped first part.
        // The binding owns any pre-admission WRITABLE retry of this Buffer;
        // appendMeasurement shares only the immutable empty tail.
        stampPayload(payload, {
          phase: 1, runId, msgSize: options.msgSize, seq: currentSeq
        });
        let submission;
        try {
          const operation = routerClient
            ? sockets[index].request(serverRoutingId)
            : sockets[index].request();
          submission = appendMeasurement(operation, payload)
            .timeout(requestTimeoutMs).submit();
        } catch (error) {
          requestFailure = error;
          break;
        }
        const task = collectReply(submission.reply);
        pending.add(task);
        task.catch((error) => { requestFailure = error; })
          .finally(() => pending.delete(task));
        if (submission.result === zlink.SubmitResult.Backpressured) {
          available[index] = false;
          const admission = submission.admitted.then(
            () => { available[index] = true; },
            (error) => { requestFailure = error; }
          ).finally(() => blocked.delete(index));
          blocked.set(index, admission);
        }
      }
      nextSocket = (sendStart + 1) % sockets.length;
      if (requestFailure) throw requestFailure;
      if (blocked.size === sockets.length) {
        await Promise.race(blocked.values());
      }
    }
    while (blocked.size > 0 && !requestFailure) {
      await Promise.race(blocked.values());
    }
    if (requestFailure) throw requestFailure;
    const drainStopNs = currentEpochNs()
      + BigInt(Math.max(1_000, requestTimeoutMs * 4)) * 1_000_000n;
    while (pending.size > 0 && currentEpochNs() < drainStopNs && !requestFailure) {
      await sleepImmediate();
    }
    if (pending.size > 0) {
      throw new Error('request completion drain timed out');
    }
    if (requestFailure) throw requestFailure;

    const result = await collector.finish();
    for (const line of summarizeMetrics(pattern, options.transport, options.msgSize,
      result.latenciesNs, options.duration, 'current', result.accepted,
      result.latencyMeanNs)) {
      console.log(line);
    }
    console.log(`CLIENT_DONE,${options.msgSize}`);
    // PERF_MULTI_TEST_POLICY.md:386-388 / PERF_POLICY.md:483-486 - keep the
    // request completion target sockets open until the runner has stopped the
    // server and sent STOP. C reference:
    // bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:722-731.
    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    let stopped = false;
    for await (const line of rl) {
      if (line === 'STOP' || line === 'QUIT') { stopped = true; break; }
    }
    if (!stopped) {
      throw new Error('runner closed stdin before STOP after CLIENT_DONE');
    }
  } finally {
    rl?.close();
    for (const socket of sockets) socket.close();
    ctx.close();
  }
}

module.exports = { runSocketReqRepClient };
