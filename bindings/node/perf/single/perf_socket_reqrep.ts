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
  applyAutoHwmMsgUnit,
  applyContextPolicy,
  applySocketPolicy,
  benchmarkEndpoint,
  configureTlsClient,
  configureTlsServer,
} = require('./perf_single_common');
const { STOP_TOKEN_BYTES, isStopToken } = require('../perf_stop_token');

const SERVER_RID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
const REQUEST_WINDOW_BYTES = 768 * 1024;
const trace = (message) => {
  if (process.env.PERF_DEBUG === '1') console.error(`[socket-reqrep] ${message}`);
};

function closeParts(parts) {
  for (const part of parts ?? []) part?.close?.();
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
      const part = received.firstPart();
      if (isStopToken(part.data())) {
        stop = true;
        received.close();
        continue;
      }
      if (received.requestSeq === null) {
        received.close();
        continue;
      }
      received.reply().message(Buffer.from(part.data())).submit();
      received.close();
    }
  } finally {
    received.close();
  }
  return stop;
}

function handshakeRouters(client, server) {
  client.send(SERVER_RID).message(Buffer.from('PING')).submit();
  const ping = new zlink.Received();
  const pong = new zlink.Received();
  try {
    server.recv(ping);
    if (!ping.routingId || ping.singlePartOrThrow().data().toString() !== 'PING') {
      throw new Error('router request/reply handshake receive failed');
    }
    server.send(ping.routingId).message(Buffer.from('PONG')).submit();
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
  const endpoint = await benchmarkEndpoint(options.transport,
    routedClient ? `router-router-reqrep-${msgSize}` : `dealer-router-reqrep-${msgSize}`);
  const poller = zlink.createPoller();
  const pollEvents = zlink.createPollEvents(1);
  try {
    applySocketPolicy(server, options);
    applySocketPolicy(client, options);
    applyAutoHwmMsgUnit(ctx, msgSize);
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
      handshakeRouters(client, server);
      trace('handshake-done');
    }
    poller.add(client, [zlink.PollEventFlag.PollCompletion], 0);

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
    let seq = 1n;
    let outstanding = 0;
    const maxInFlight = Math.max(
      1,
      Math.min(64, Math.floor(REQUEST_WINDOW_BYTES / Math.max(1, msgSize)))
    );
    let failure = null;
    const callback = (result, parts) => {
      try {
        if (result === zlink.RequestResult.Ok && parts?.length > 0) {
          collector.recordPayload(parts[0].data(), currentEpochNs());
        } else if (result !== zlink.RequestResult.TimedOut) {
          failure = new Error(`request completion failed: ${result}`);
        }
      } catch (error) {
        failure = error;
      } finally {
        closeParts(parts);
        outstanding -= 1;
      }
    };

    while (currentEpochNs() < activeStopNs && !failure) {
      while (outstanding < maxInFlight && currentEpochNs() < activeStopNs) {
        const payload = createPayload(msgSize);
        stampPayload(payload, { phase: 1, runId, msgSize, seq });
        try {
          const operation = routedClient ? client.request(SERVER_RID) : client.request();
          const accepted = operation.message(payload)
            .timeout(options.recvTimeoutMs ?? 200)
            .flags(zlink.SendFlags.None)
            .submit(callback);
          if (accepted) {
            outstanding += 1;
            seq += 1n;
          } else {
            break;
          }
        } catch (error) {
          if (!transientSubmit(error)) throw error;
          break;
        }
      }
      drainServer(server);
      poller.wait(pollEvents, 0);
      await sleepImmediate();
    }
    trace(`active-done outstanding=${outstanding}`);

    const drainStopNs = currentEpochNs() + 10_000_000_000n;
    while (outstanding > 0 && currentEpochNs() < drainStopNs) {
      drainServer(server);
      poller.wait(pollEvents, 10);
      await sleepImmediate();
    }
    if (failure || outstanding !== 0) throw failure ?? new Error('request drain timed out');
    trace('drain-done');

    const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
    stopOperation.message(STOP_TOKEN_BYTES).flags(zlink.SendFlags.None).submit();
    const stopDeadlineNs = currentEpochNs() + 5_000_000_000n;
    let stopReceived = false;
    while (!stopReceived && currentEpochNs() < stopDeadlineNs) {
      stopReceived = drainServer(server);
      if (!stopReceived) await sleepImmediate();
    }
    if (!stopReceived) throw new Error('wire stop token was not received');
    trace('stop-done');
    return collector.finish();
  } catch (error) {
    trace(`failure=${error?.stack ?? error}`);
    throw error;
  } finally {
    trace('closing');
    for (const resource of [pollEvents, poller, clientMonitor, serverMonitor,
      client, server, ctx]) {
      try { resource?.close?.(); } catch (_) { /* preserve the benchmark failure */ }
    }
  }
}

module.exports = { runSocketReqRep };
