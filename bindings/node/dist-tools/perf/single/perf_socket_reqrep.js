// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, currentEpochNs, sleepImmediate, stampPayload, } = require('../common/perf_metrics');
const { applyContextPolicy, applySocketPolicy, benchmarkEndpoint, configureTlsClient, configureTlsServer, } = require('./perf_single_common');
const { STOP_TOKEN_BYTES, isStopToken } = require('../perf_stop_token');
const SERVER_RID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
const REQUEST_WINDOW_BYTES = 768 * 1024;
const trace = (message) => {
    if (process.env.PERF_DEBUG === '1')
        console.error(`[socket-reqrep] ${message}`);
};
function closeParts(parts) {
    for (const part of parts ?? [])
        part?.close?.();
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
    }
    catch (error) {
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
    }
    finally {
        activity.close();
    }
    throw new Error(`connection ready timeout after ${timeoutMs}ms server=${serverReady} client=${clientReady}`);
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
    }
    finally {
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
    }
    finally {
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
    const endpoint = await benchmarkEndpoint(options.transport, routedClient ? `router-router-reqrep-${msgSize}` : `dealer-router-reqrep-${msgSize}`);
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
        const maxInFlight = Math.max(1, Math.min(64, Math.floor(REQUEST_WINDOW_BYTES / Math.max(1, msgSize))));
        let failure = null;
        let completionSignal = Promise.resolve();
        const observe = async (request) => {
            let parts = null;
            try {
                parts = await request;
                if (parts?.length > 0) {
                    collector.recordPayload(parts[0].data(), currentEpochNs());
                }
            }
            catch (error) {
                if (!(error instanceof zlink.RequestError
                    && error.result === zlink.RequestResult.TimedOut)) {
                    failure = error;
                }
            }
            finally {
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
                    const request = operation.message(payload)
                        .timeout(options.recvTimeoutMs ?? 200).submit();
                    outstanding += 1;
                    completionSignal = observe(request);
                    seq += 1n;
                }
                catch (error) {
                    if (!transientSubmit(error))
                        throw error;
                    break;
                }
            }
            drainServer(server);
            await Promise.race([completionSignal, sleepImmediate()]);
        }
        trace(`active-done outstanding=${outstanding}`);
        const drainStopNs = currentEpochNs() + 10000000000n;
        while (outstanding > 0 && currentEpochNs() < drainStopNs) {
            drainServer(server);
            await Promise.race([completionSignal, sleepImmediate()]);
        }
        if (failure || outstanding !== 0)
            throw failure ?? new Error('request drain timed out');
        trace('drain-done');
        const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
        await stopOperation.message(STOP_TOKEN_BYTES).submit();
        const stopDeadlineNs = currentEpochNs() + 5000000000n;
        let stopReceived = false;
        while (!stopReceived && currentEpochNs() < stopDeadlineNs) {
            stopReceived = drainServer(server);
            if (!stopReceived)
                await sleepImmediate();
        }
        if (!stopReceived)
            throw new Error('wire stop token was not received');
        trace('stop-done');
        return collector.finish();
    }
    catch (error) {
        trace(`failure=${error?.stack ?? error}`);
        throw error;
    }
    finally {
        trace('closing');
        for (const resource of [clientMonitor, serverMonitor,
            client, server, ctx]) {
            try {
                resource?.close?.();
            }
            catch (_) { /* preserve the benchmark failure */ }
        }
    }
}
module.exports = { runSocketReqRep };
