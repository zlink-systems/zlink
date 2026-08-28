// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createPayload,
  createRunId,
  currentEpochNs,
  stampPayload,
} = require('../common/perf_metrics');
const {
  applyContextPolicy,
  applySocketPolicy,
  benchmarkEndpoint,
  closeSenderWorker,
  configureTlsClient,
  releaseSenderWorker,
  spawnSenderWorker,
  waitForMonitorConnectionReady,
  waitForWorkerStatus,
} = require('./perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');

const SERVER_RID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));

function closeParts(parts) {
  for (const part of parts ?? []) part?.close?.();
}

function appendMeasurement(op, payload) {
  op = op.message(payload);
  if (process.env.PERF_PART_COUNT !== '1') op = op.message(Buffer.alloc(0));
  return op;
}

function measurementPayload(parts) {
  const count = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
  if (!Array.isArray(parts) || parts.length !== count) return null;
  if (count === 2 && parts[1].data().length !== 0) return null;
  return parts[0];
}

function transientSubmit(error) {
  return error instanceof zlink.SubmitError
    && (error.result === zlink.SubmitResult.Backpressured
      || error.result === zlink.SubmitResult.NotConnected
      || error.result === zlink.SubmitResult.NotFound);
}

function requestOperation(client, routedClient, payload, timeoutMs) {
  const operation = routedClient ? client.request(SERVER_RID) : client.request();
  return appendMeasurement(operation, payload).timeout(timeoutMs);
}

function routingProbe(client, routedClient, timeoutMs) {
  const expected = Buffer.from('__zlink_perf_reqrep_probe__');
  const parts = requestOperation(client, routedClient, expected, timeoutMs)
    .submit_sync(zlink.SendFlags.None);
  try {
    const payload = measurementPayload(parts);
    return payload !== null && payload.data().equals(expected);
  } finally {
    closeParts(parts);
  }
}

async function runSocketReqRep(msgSize, options, routedClient) {
  const endpoint = await benchmarkEndpoint(
    options.transport,
    routedClient ? `router-router-reqrep-${msgSize}` : `dealer-router-reqrep-${msgSize}`
  );
  const ctx = zlink.createContext();
  applyContextPolicy(ctx);
  const client = routedClient ? zlink.createRouterSocket(ctx)
                              : zlink.createDealerSocket(ctx);
  const clientMonitor = client.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
  const completionPoller = zlink.createPoller();
  const completionEvents = zlink.createPollEvents(1);
  let worker = null;

  try {
    applySocketPolicy(client, options);
    if (routedClient) {
      client.setRoutingId(zlink.RoutingId.from(Buffer.from('CLIENT', 'ascii')));
      client.options.setConnectRoutingId(SERVER_RID);
      client.options.mandatory = true;
    }
    ctx.recalculateAutoHwm();
    configureTlsClient(client, options.transport);
    worker = spawnSenderWorker({
      kind: 'socket_reqrep_replier',
      transport: options.transport,
      endpoint,
      duration: options.duration,
      msgSize,
      runId: options.runId ?? 1,
      options,
    });
    waitForWorkerStatus(worker, 1);
    client.connect(endpoint);
    waitForMonitorConnectionReady(clientMonitor);
    releaseSenderWorker(worker);

    const requestTimeoutMs = Number.isFinite(options.recvTimeoutMs)
      ? Math.trunc(options.recvTimeoutMs)
      : 200;
    if (!routingProbe(client, routedClient, requestTimeoutMs)) {
      throw new Error('request-reply routing probe failed');
    }

    completionPoller.add(client, [zlink.PollEventFlag.PollCompletion], 0);
    const runId = createRunId(options.runId ?? 1);
    const activeStartNs = currentEpochNs();
    const activeStopNs = activeStartNs
      + BigInt(Math.floor(options.duration * 1_000_000_000));
    const collector = createMetricCollector({
      runId,
      msgSize,
      activeStartNs,
      activeStopNs,
      roundTrip: false,
    });
    const payload = createPayload(msgSize);
    let seq = 1n;
    let outstanding = 0;
    let failure = null;
    const observe = (error, parts) => {
      try {
        if (error) throw error;
        const replyPayload = measurementPayload(parts);
        collector.recordPayload(replyPayload ? replyPayload.data() : null, currentEpochNs());
      } catch (error) {
        if (!(error instanceof zlink.RequestError
            && error.result === zlink.RequestResult.TimedOut)) {
          failure = error;
        }
      } finally {
        closeParts(parts);
        outstanding -= 1;
      }
    };

    while (currentEpochNs() < activeStopNs && !failure) {
      stampPayload(payload, { phase: 1, runId, msgSize, seq });
      // A completion may be delivered inline from the same synchronous
      // requester thread, so reserve the count before transferring ownership.
      outstanding += 1;
      let backpressured = false;
      try {
        requestOperation(client, routedClient, payload, requestTimeoutMs)
          .submit_sync(zlink.SendFlags.DontWait, observe);
        seq += 1n;
      } catch (error) {
        outstanding -= 1;
        if (!transientSubmit(error)) throw error;
        backpressured = true;
      }
      // PollCompletion is the public synchronous completion-progress surface;
      // its callback is delivered before wait() returns, without an event-loop
      // or Promise hop.
      completionPoller.wait(completionEvents, backpressured ? 25 : 0);
    }

    const drainStopNs = currentEpochNs() + 10_000_000_000n;
    while (outstanding > 0 && currentEpochNs() < drainStopNs && !failure) {
      completionPoller.wait(completionEvents, 25);
    }
    if (failure || outstanding !== 0) {
      throw failure ?? new Error('request drain timed out');
    }

    const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
    stopOperation.message(STOP_TOKEN_BYTES).submit_sync(zlink.SendFlags.None);
    waitForWorkerStatus(worker, 4, 10_000);
    return collector.finish();
  } finally {
    await closeSenderWorker(worker);
    for (const resource of [completionEvents, completionPoller, clientMonitor, client, ctx]) {
      try { resource?.close?.(); } catch (_) { /* preserve the benchmark failure */ }
    }
  }
}

module.exports = { runSocketReqRep };
