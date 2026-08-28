// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createRunId,
  currentEpochNs,
  HEADER_SIZE,
  summarizeMetrics,
} = require('../common/perf_metrics');
const {
  applyContextPolicy,
  applySocketPolicy,
  appendMeasurement,
  benchmarkEndpoint,
  closeSenderWorker,
  configureTlsClient,
  drainRecvSocket,
  emitSingleSocketHwmDetail,
  measurementPayload,
  parseSingleBinaryArgs,
  runLocalSocketOneWayBenchmark,
  releaseSenderWorker,
  spawnSenderWorker,
  waitForPostReadySettle,
  waitForMonitorConnectionReady,
  waitForWorkerStatus,
} = require('./perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');

function trace(message) {
  if (process.env.PERF_NODE_TRACE === '1') {
    console.error(`[pubsub] ${message}`);
  }
}

function trySubscribe(socket, received, buffer, flags) {
  try {
    if (!socket.subscribe(received, flags)) {
      return null;
    }
    if (received.parts.length === 1 && received.parts[0].data().equals(STOP_TOKEN_BYTES)) {
      return { stop: true, size: STOP_TOKEN_BYTES.length };
    }
    const payload = measurementPayload(received.parts);
    if (!payload) {
      return { valid: false, size: 0 };
    }
    const data = payload.data();
    data.copy(buffer, 0, 0, Math.min(buffer.length, data.length));
    return { valid: true, size: data.length };
  } catch (error) {
    if (error instanceof zlink.RecvError &&
        (error.result === zlink.RecvResult.NoData || error.nativeErrno === 2)) {
      return null;
    }
    throw error;
  }
}

function drainPubSubPayloadInto(
  socket,
  buffer,
  onHeader,
  options: { recordUntilNs?: bigint | number | string } = {}
) {
  const recordUntilNs = options.recordUntilNs === undefined
    ? null
    : BigInt(options.recordUntilNs);
  let stopReceived = false;
  let recordingActive = true;
  const reusableReceived = new zlink.TopicMessage();
  while (!stopReceived) {
    let first = true;
    while (true) {
      const received = trySubscribe(
        socket,
        reusableReceived,
        buffer,
        first ? zlink.RecvFlags.None : zlink.RecvFlags.DontWait
      );
      if (!received) {
        break;
      }
      first = false;
      if (received.stop) {
        stopReceived = true;
        break;
      }
      if (!received.valid) {
        onHeader(null, currentEpochNs());
        continue;
      }
      if (recordUntilNs !== null && recordingActive) {
        recordingActive = currentEpochNs() <= recordUntilNs;
      }
      if (recordUntilNs !== null && !recordingActive) {
        continue;
      }
      if (received.size < HEADER_SIZE) {
        onHeader(null, currentEpochNs());
        continue;
      }
      onHeader(buffer, currentEpochNs(), received.size);
    }
  }
}

async function runPubSubBenchmark(msgSize, options) {
  if (options.transport === 'inproc') {
    // inproc is context-local (doc/guide/04-transports.md) so the Worker
    // sender path cannot reach it. Run PUB+SUB in one shared context with
    // the C-faithful blocking-equivalent one-way model (same adaptation
    // as PAIR/DEALER inproc). C perf_pubsub.cpp: PUB binds, SUB connects
    // + subscribes; wire stop token on the topic ends the phase.
    const TOPIC = 'bench';
    return runLocalSocketOneWayBenchmark({
      pattern: 'PUBSUB',
      msgSize,
      options,
      endpointToken: 'pubsub',
      createReceiver: (ctx) => zlink.createSubSocket(ctx),
      createSender: (ctx) => zlink.createPubSocket(ctx),
      configureReceiver: (socket) => socket.setSubscription(TOPIC),
      senderBinds: true,
      drainViaSubscribe: true,
      // C perf_pubsub.cpp emits AUTO_HWM_DETAIL for the PUB as
      // `publisher` and the SUB as `subscriber`; mirror those labels so
      // the `## Auto-HWM Detail` PUBSUB block is byte-identical to C
      // (receiver=SUB=subscriber, sender=PUB=publisher).
      receiverHwmComponent: 'subscriber',
      senderHwmComponent: 'publisher',
      sendActive: (socket, payload) => {
        try {
          appendMeasurement(socket.publish(TOPIC), payload).submit();
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
      sendStop: (socket) => {
        socket.publish(TOPIC).message(STOP_TOKEN_BYTES).submit();
      },
    });
  }

  const ctx = zlink.createContext();
  applyContextPolicy(ctx);
  const sub = zlink.createSubSocket(ctx);
  const subMonitor = sub.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
  const endpoint = await benchmarkEndpoint(options.transport, `pubsub-${msgSize}`);
  let worker = null;

  try {
    applySocketPolicy(sub, {
      ...options,
      recvTimeout: Number(
        process.env.PERF_SINGLE_PUBSUB_RCVTIMEO_MS
        ?? process.env.PERF_SINGLE_RCVTIMEO_MS
        ?? 200
      )
    });
    ctx.recalculateAutoHwm();
    // The SUB connects from this (main) process, so it must carry the TLS
    // client config for tls/wss exactly like the worker's connecting
    // sockets do (perf_single_sender_worker.ts:51). Without it the SUB
    // cannot complete the TLS handshake with the TLS PUB bind and the
    // CONNECTION_READY gate never fires (PUBSUB tls/wss hung pre-data).
    configureTlsClient(sub, options.transport);
    sub.setSubscription('');
    sub.connect(endpoint);
    worker = spawnSenderWorker({
      kind: 'pubsub',
      transport: options.transport,
      endpoint,
      duration: options.duration,
      msgSize,
      runId: options.runId ?? 1,
      topic: 'bench',
      options: {
        ...options,
        noDrop: process.env.PERF_SINGLE_PUBSUB_XPUB_NODROP === '0'
          ? false
          : true
      },
    });
    waitForWorkerStatus(worker, 1);
    // C setup_connected_pubsub_pair: wait for the subscriber
    // CONNECTION_READY and the post-ready settle BEFORE releasing the
    // publisher's active loop, so the active window starts on a settled
    // connected pair (not an extra start gate — this is the ready gate).
    waitForMonitorConnectionReady(subMonitor);
    waitForWorkerStatus(worker, 2);
    trace('connection ready');
    waitForPostReadySettle(Number(process.env.PERF_SINGLE_PUBSUB_READY_SETTLE_MS ?? 1000));
    releaseSenderWorker(worker);

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

    // PERF_SINGLE_TEST_POLICY § 1.4 / § 2.0.1 / C setup_connected_pubsub_
    // pair + send_pubsub_stop_token: the `bound`->CONNECTION_READY->settle
    // ->`ready` exchange above IS the connection-ready gate (PUB binds in
    // the worker, SUB connects here). No extra start/stop ack — the
    // subscriber drains until the wire stop token on the topic.
    const payloadBuffer = Buffer.allocUnsafe(Math.max(msgSize, HEADER_SIZE, STOP_TOKEN_BYTES.length));
    drainPubSubPayloadInto(
      sub,
      payloadBuffer,
      (payload, receivedAtNs, receivedSize) => {
        if (receivedSize !== Math.max(msgSize, HEADER_SIZE)) {
          collector.recordPayload(null, receivedAtNs);
          return;
        }
        collector.recordPayload(payload, receivedAtNs);
      },
      { recordUntilNs: activeStopNs }
    );
    waitForWorkerStatus(worker, 4);
    const result = collector.finish();
    emitSingleSocketHwmDetail(sub, 'PUBSUB', options.transport, 'subscriber', msgSize);
    return result;
  } finally {
    trace('closing');
    await closeSenderWorker(worker);
    subMonitor.close();
    sub.close();
    trace('sub closed');
    ctx.close();
    trace('ctx closed');
  }
}

module.exports = { runPubSubBenchmark };

if (require.main === module) {
  (async () => {
    const options = parseSingleBinaryArgs(process.argv.slice(2));
    const result = await runPubSubBenchmark(options.msgSize, options);
    if (result.unsupported) {
      console.log(`UNSUPPORTED,${options.libName},PUBSUB,${options.transport}`);
      return;
    }
    for (const line of summarizeMetrics(
      'PUBSUB',
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
