// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createPayload,
  createRunId,
  currentEpochNs,
  sleepImmediate,
  stampPayload,
} = require('../common/perf_metrics');
const {
  applyContextPolicy,
  applySocketPolicy,
  benchmarkEndpoint,
  configureTlsClient,
  configureTlsServer,
} = require('./perf_single_common');
const { STOP_TOKEN_BYTES, isStopToken } = require('../perf_stop_token');

const SERVER_RID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
const trace = (message) => {
  if (process.env.PERF_DEBUG === '1') console.error(`[socket-reqrep] ${message}`);
};

function closeParts(parts) {
  for (const part of parts ?? []) part?.close?.();
}

function appendMeasurement(op, payload) {
  op = op.message(payload);
  if (process.env.PERF_PART_COUNT !== '1') {
    op = op.message(Buffer.alloc(0));
  }
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

function monitorReady(monitor) {
  try {
    return monitor.recv(zlink.RecvFlags.DontWait)?.event
      === zlink.MonitorEventType.ConnectionReady;
  } catch (error) {
    if (error instanceof zlink.RecvError
        && error.result === zlink.RecvResult.NoData) {
      return false;
    }
    throw error;
  }
}

async function waitForRequestSocketsReady(server, serverMonitor, clientMonitor) {
  const timeoutMs = Math.max(1, Number(process.env.PERF_CONNECT_READY_TIMEOUT_MS ?? 1000));
  const deadline = Date.now() + timeoutMs;
  const activity = new zlink.Received();
  let serverReady = false;
  let clientReady = false;
  try {
    while (Date.now() < deadline) {
      server.recv(activity, zlink.RecvFlags.DontWait);
      activity.close();
      serverReady = serverReady || monitorReady(serverMonitor);
      clientReady = clientReady || monitorReady(clientMonitor);
      if (serverReady && clientReady) {
        return;
      }
      await sleepImmediate();
    }
  } finally {
    activity.close();
  }
  throw new Error(
    `connection ready timeout after ${timeoutMs}ms server=${serverReady} client=${clientReady}`
  );
}

function drainServer(server) {
  let stop = false;
  const received = new zlink.Received();
  try {
    while (server.recv(received, zlink.RecvFlags.DontWait)) {
      const parts = received.parts;
      if (parts.length === 1 && isStopToken(parts[0].data())) {
        stop = true;
        received.close();
        continue;
      }
      if (received.requestSeq === null) {
        received.close();
        continue;
      }
      const payload = measurementPayload(parts);
      if (!payload) {
        received.close();
        continue;
      }
      appendMeasurement(received.reply(), Buffer.from(payload.data())).submit();
      received.close();
    }
  } finally {
    received.close();
  }
  return stop;
}

async function handshakeRouters(client, server) {
  await client.send(SERVER_RID).message(Buffer.from('PING')).submit();
  const ping = new zlink.Received();
  const pong = new zlink.Received();
  try {
    server.recv(ping);
    if (!ping.routingId || ping.singlePartOrThrow().data().toString() !== 'PING') {
      throw new Error('router request/reply handshake receive failed');
    }
    await server.send(ping.routingId).message(Buffer.from('PONG')).submit();
    client.recv(pong);
    if (pong.singlePartOrThrow().data().toString() !== 'PONG') {
      throw new Error('router request/reply handshake reply failed');
    }
  } finally {
    pong.close();
    ping.close();
  }
}

async function runSocketReqRep(msgSize, options, routedClient) {
  const ctx = zlink.createContext();
  applyContextPolicy(ctx);
  const server = zlink.createRouterSocket(ctx);
  const client = routedClient ? zlink.createRouterSocket(ctx)
                              : zlink.createDealerSocket(ctx);
  const serverMonitor = server.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
  const clientMonitor = client.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
  const completionPoller = zlink.createPoller();
  const completionEvents = zlink.createPollEvents(1);
  const endpoint = await benchmarkEndpoint(options.transport,
    routedClient ? `router-router-reqrep-${msgSize}` : `dealer-router-reqrep-${msgSize}`);
  let replierTask = null;
  let stopReplier = false;
  try {
    applySocketPolicy(server, options);
    applySocketPolicy(client, options);
    server.setRoutingId(SERVER_RID);
    server.options.mandatory = true;
    if (routedClient) {
      client.setRoutingId(zlink.RoutingId.from(Buffer.from('CLIENT', 'ascii')));
      client.options.setConnectRoutingId(SERVER_RID);
      client.options.mandatory = true;
    }
    configureTlsServer(server, options.transport);
    configureTlsClient(client, options.transport);
    server.bind(endpoint);
    client.connect(endpoint);
    trace('connected-called');
    await waitForRequestSocketsReady(server, serverMonitor, clientMonitor);
    ctx.recalculateAutoHwm();
    trace('ready');
    if (routedClient) {
      await handshakeRouters(client, server);
      trace('handshake-done');
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
      acceptDrainCompletions: true,
    });
    let seq = 1n;
    let outstanding = 0;
    // parseSingleBinaryArgs yields NaN when PERF_SINGLE_RCVTIMEO_MS is unset,
    // and NaN is not nullish, so `??` would forward it into timeout().
    const requestTimeoutMs = Number.isFinite(options.recvTimeoutMs)
      ? Math.trunc(options.recvTimeoutMs)
      : 200;
    let failure = null;
    let replierFailure = null;
    let stopReceived = false;
    const observe = (error, parts) => {
      try {
        if (error) throw error;
        const payload = measurementPayload(parts);
        if (payload) {
          collector.recordPayload(payload.data(), currentEpochNs());
        } else {
          collector.recordPayload(null, currentEpochNs());
        }
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

    // Keep reply receive/reply work in an independent event-loop flow. The
    // requester flow below can continue submitting and progressing completion
    // callbacks while this flow drains the replier socket, matching C's three
    // simultaneously active submit/progress/replier roles as closely as the
    // single-threaded public Node runtime permits.
    replierTask = (async () => {
      while (!stopReplier) {
        try {
          if (drainServer(server)) {
            stopReceived = true;
            return;
          }
        } catch (error) {
          replierFailure = error;
          return;
        }
        await sleepImmediate();
      }
    })();

    while (currentEpochNs() < activeStopNs && !failure && !replierFailure) {
      const payload = createPayload(msgSize);
      stampPayload(payload, { phase: 1, runId, msgSize, seq });
      try {
        const operation = routedClient ? client.request(SERVER_RID) : client.request();
        appendMeasurement(operation, payload).timeout(requestTimeoutMs)
          .submit_sync(zlink.SendFlags.DontWait, observe);
        outstanding += 1;
        seq += 1n;
      } catch (error) {
        if (!transientSubmit(error)) throw error;
      }
      // Let native request completions and admission failures dispatch. The
      // request surface owns its pending limit; the runner does not impose an
      // inflight/window cap.
      completionPoller.wait(completionEvents, 0);
      await sleepImmediate();
    }
    trace(`active-done outstanding=${outstanding}`);

    const drainStopNs = currentEpochNs() + 10_000_000_000n;
    while (outstanding > 0 && currentEpochNs() < drainStopNs && !replierFailure) {
      completionPoller.wait(completionEvents, 0);
      await sleepImmediate();
    }
    if (failure || replierFailure || outstanding !== 0) {
      stopReplier = true;
      await replierTask;
      throw failure ?? replierFailure ?? new Error('request drain timed out');
    }
    trace('drain-done');

    const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
    await stopOperation.message(STOP_TOKEN_BYTES).submit();
    const stopDeadlineNs = currentEpochNs() + 5_000_000_000n;
    while (!stopReceived && currentEpochNs() < stopDeadlineNs) {
      await Promise.race([replierTask, sleepImmediate()]);
    }
    stopReplier = true;
    await replierTask;
    if (replierFailure) throw replierFailure;
    if (!stopReceived) throw new Error('wire stop token was not received');
    trace('stop-done');
    return collector.finish();
  } catch (error) {
    stopReplier = true;
    await replierTask;
    trace(`failure=${error?.stack ?? error}`);
    throw error;
  } finally {
    stopReplier = true;
    await replierTask?.catch?.(() => {});
    trace('closing');
    for (const resource of [completionEvents, completionPoller, clientMonitor, serverMonitor,
      client, server, ctx]) {
      try { resource?.close?.(); } catch (_) { /* preserve the benchmark failure */ }
    }
  }
}

module.exports = { runSocketReqRep };
