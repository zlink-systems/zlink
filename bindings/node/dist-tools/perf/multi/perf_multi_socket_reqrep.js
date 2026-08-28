// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, currentEpochNs, sleepImmediate, stampPayload, summarizeMetrics } = require('../common/perf_metrics');
const { configureTlsClient } = require('../common/perf_tls');
const { applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, waitForConnectionReady } = require('./perf_multi_runtime');
function appendMeasurement(op, payload) {
    op = op.message(payload);
    if (process.env.PERF_PART_COUNT !== '1')
        op = op.message(Buffer.alloc(0));
    return op;
}
function measurementPayload(parts) {
    const count = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
    if (!Array.isArray(parts) || parts.length !== count)
        return null;
    if (count === 2 && parts[1].data().length !== 0)
        return null;
    return parts[0];
}
function closeParts(parts) {
    for (const part of parts ?? [])
        part?.close?.();
}
function transientRequest(error) {
    return error instanceof zlink.SubmitError
        && (error.result === zlink.SubmitResult.Backpressured
            || error.result === zlink.SubmitResult.NotConnected
            || error.result === zlink.SubmitResult.NotFound);
}
async function runSocketReqRepClient({ options, pattern, routerClient, serverRoutingId }) {
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'client', pattern);
    const sockets = [];
    const poller = zlink.createPoller();
    const pollBuffer = zlink.createPollEvents(Math.max(1, options.clients));
    const outstanding = new Array(options.clients).fill(0);
    const blocked = new Array(options.clients).fill(false);
    let fatal = null;
    let rl = null;
    try {
        for (let i = 0; i < options.clients; i += 1) {
            const socket = routerClient ? zlink.createRouterSocket(ctx) : zlink.createDealerSocket(ctx);
            applySocketPolicy(socket, { transport: options.transport });
            configureTlsClient(socket, options.transport);
            socket.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${i}`, 'ascii')));
            if (routerClient)
                socket.options.setConnectRoutingId(serverRoutingId);
            sockets.push(socket);
            await waitForConnectionReady(socket, () => socket.connect(options.endpoint));
            // Completion owns this registration. POLLIN/POLLOUT must not be mixed in.
            poller.add(socket, pollEvents(zlink.PollEventFlag.PollCompletion), i);
        }
        ctx.recalculateAutoHwm();
        for (const socket of sockets) {
            emitMultiSocketHwmDetail(socket, 'endpoint', options.transport, options.msgSize);
        }
        console.log(`CLIENT_READY,${options.msgSize}`);
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        for await (const line of rl) {
            if (line === `START,${options.msgSize}`)
                break;
            if (line === 'STOP' || line === 'QUIT')
                return;
        }
        const runId = createRunId(1);
        const activeStartNs = currentEpochNs();
        const activeStopNs = activeStartNs
            + BigInt(Math.floor(options.duration * 1_000_000_000));
        const collector = createMetricCollector({
            suite: 'multi', runId, msgSize: options.msgSize,
            activeStartNs, activeStopNs, roundTrip: true, acceptDrainCompletions: true
        });
        const requestTimeoutMs = Math.max(1, Number(process.env.PERF_MULTI_REQREP_TIMEOUT_MS ?? 200));
        let seq = 1n;
        const observe = (index, error, parts) => {
            try {
                if (error)
                    throw error;
                const payload = measurementPayload(parts);
                collector.recordPayload(payload?.data?.() ?? null, currentEpochNs());
                blocked[index] = false;
            }
            catch (error) {
                if (transientRequest(error)
                    || (error instanceof zlink.RequestError
                        && error.result === zlink.RequestResult.TimedOut)) {
                    blocked[index] = true;
                }
                else {
                    fatal = error;
                }
            }
            finally {
                closeParts(parts);
                outstanding[index] -= 1;
            }
        };
        while (currentEpochNs() < activeStopNs && !fatal) {
            let submitted = false;
            for (let i = 0; i < sockets.length && currentEpochNs() < activeStopNs; i += 1) {
                if (blocked[i])
                    continue;
                while (currentEpochNs() < activeStopNs) {
                    const payload = createPayload(options.msgSize);
                    stampPayload(payload, { phase: 1, runId, msgSize: options.msgSize, seq });
                    const operation = routerClient ? sockets[i].request(serverRoutingId) : sockets[i].request();
                    try {
                        appendMeasurement(operation, payload).timeout(requestTimeoutMs)
                            .submit_sync(zlink.SendFlags.DontWait, (error, parts) => observe(i, error, parts));
                        outstanding[i] += 1;
                        submitted = true;
                        seq += 1n;
                    }
                    catch (error) {
                        if (!transientRequest(error))
                            throw error;
                        blocked[i] = true;
                        break;
                    }
                }
            }
            // The external completion poller owns native completion progress. Node
            // needs one event-loop turn after the drain to deliver Promise callbacks.
            poller.wait(pollBuffer, submitted ? 0 : 50);
            await sleepImmediate();
            if (!submitted)
                blocked.fill(false);
        }
        const drainStopNs = currentEpochNs()
            + BigInt(Math.max(1000, requestTimeoutMs * 4)) * 1000000n;
        while (outstanding.some((count) => count > 0) && currentEpochNs() < drainStopNs && !fatal) {
            poller.wait(pollBuffer, 50);
            await sleepImmediate();
        }
        if (fatal)
            throw fatal;
        if (outstanding.some((count) => count > 0))
            throw new Error('request drain timed out');
        const result = await collector.finish();
        for (const line of summarizeMetrics(pattern, options.transport, options.msgSize, result.latenciesNs, options.duration, 'current', result.accepted, result.latencyMeanNs)) {
            console.log(line);
        }
        console.log(`CLIENT_DONE,${options.msgSize}`);
    }
    finally {
        rl?.close();
        pollBuffer.close();
        poller.close();
        for (const socket of sockets)
            socket.close();
        ctx.close();
    }
}
module.exports = { runSocketReqRepClient };
