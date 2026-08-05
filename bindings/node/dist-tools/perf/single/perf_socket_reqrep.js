// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, currentEpochNs, sleepImmediate, stampPayload, } = require('../common/perf_metrics');
const { applyAutoHwmMsgUnit, applyContextPolicy, applySocketPolicy, benchmarkEndpoint, configureTlsClient, configureTlsServer, waitForMonitorConnectionReady, } = require('./perf_single_common');
const { STOP_TOKEN_BYTES, isStopToken } = require('../perf_stop_token');
const SERVER_RID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
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
async function runSocketReqRep(msgSize, options, routedClient) {
    const ctx = zlink.createContext();
    applyContextPolicy(ctx);
    const server = zlink.createRouterSocket(ctx);
    const client = routedClient ? zlink.createRouterSocket(ctx)
        : zlink.createDealerSocket(ctx);
    const serverMonitor = server.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const clientMonitor = client.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const endpoint = await benchmarkEndpoint(options.transport, routedClient ? `router-router-reqrep-${msgSize}` : `dealer-router-reqrep-${msgSize}`);
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
            client.options.mandatory = true;
        }
        configureTlsServer(server, options.transport);
        configureTlsClient(client, options.transport);
        server.bind(endpoint);
        client.connect(endpoint);
        trace('connected-called');
        await Promise.all([
            waitForMonitorConnectionReady(serverMonitor),
            waitForMonitorConnectionReady(clientMonitor),
        ]);
        ctx.recalculateAutoHwm();
        trace('ready');
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
        let failure = null;
        const callback = (result, parts) => {
            try {
                if (result === zlink.RequestResult.Ok && parts?.length > 0) {
                    collector.recordPayload(parts[0].data(), currentEpochNs());
                }
                else if (result !== zlink.RequestResult.TimedOut) {
                    failure = new Error(`request completion failed: ${result}`);
                }
            }
            catch (error) {
                failure = error;
            }
            finally {
                closeParts(parts);
                outstanding -= 1;
            }
        };
        while (currentEpochNs() < activeStopNs && !failure) {
            if (outstanding === 0) {
                const payload = createPayload(msgSize);
                stampPayload(payload, { phase: 1, runId, msgSize, seq });
                try {
                    const operation = routedClient ? client.request(SERVER_RID) : client.request();
                    const accepted = operation.message(payload)
                        .timeout(options.recvTimeoutMs ?? 200)
                        .flags(zlink.SendFlags.DontWait)
                        .submit(callback);
                    if (accepted) {
                        outstanding = 1;
                        seq += 1n;
                    }
                }
                catch (error) {
                    if (!transientSubmit(error))
                        throw error;
                }
            }
            drainServer(server);
            poller.wait(pollEvents, 0);
            await sleepImmediate();
        }
        trace(`active-done outstanding=${outstanding}`);
        const drainStopNs = currentEpochNs() + 10000000000n;
        while (outstanding > 0 && currentEpochNs() < drainStopNs) {
            drainServer(server);
            poller.wait(pollEvents, 10);
            await sleepImmediate();
        }
        if (failure || outstanding !== 0)
            throw failure ?? new Error('request drain timed out');
        trace('drain-done');
        const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
        stopOperation.message(STOP_TOKEN_BYTES).flags(zlink.SendFlags.None).submit();
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
        for (const resource of [pollEvents, poller, clientMonitor, serverMonitor,
            client, server, ctx]) {
            try {
                resource?.close?.();
            }
            catch (_) { /* preserve the benchmark failure */ }
        }
    }
}
module.exports = { runSocketReqRep };
