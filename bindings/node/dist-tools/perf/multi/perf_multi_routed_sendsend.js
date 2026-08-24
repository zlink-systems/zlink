// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, HEADER_SIZE, currentEpochNs, stampPayload, summarizeMetrics } = require('../common/perf_metrics');
const { configureTlsClient, configureTlsServer } = require('../common/perf_tls');
const { STOP_TOKEN_BYTES, isStopToken } = require('../perf_stop_token');
const { parseMultiArgs } = require('./perf_multi_common');
const { POLLIN, POLLOUT, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, pollEventHas, tryRoutedSocketSend, waitForConnectionReady, waitForConnectionReadyCount, waitPollerOne } = require('./perf_multi_runtime');
const SERVER_ROUTING_ID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
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
        ? tryRoutedSocketSend(socket, SERVER_ROUTING_ID, payload)
        : tryRoutedSocketSend(socket, payload);
}
async function sendStopTokenWithRetry(socket, routerClient, poller, pollBuffer) {
    const deadline = Date.now() + 5000;
    while (!(await sendPayload(socket, routerClient, STOP_TOKEN_BYTES))) {
        if (Date.now() >= deadline) {
            throw new Error('stop token send timeout');
        }
        poller.wait(pollBuffer, 50);
    }
}
async function runRoutedSendSendClient({ options, pattern, routerClient }) {
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'client', pattern);
    const sockets = [];
    const payloads = [];
    const pending = [];
    const poller = zlink.createPoller();
    const pollBuffer = zlink.createPollEvents(Math.max(1, options.clients));
    let rl = null;
    try {
        for (let i = 0; i < options.clients; i += 1) {
            const socket = createClientSocket(ctx, routerClient);
            applySocketPolicy(socket, { transport: options.transport });
            configureTlsClient(socket, options.transport);
            if (routerClient) {
                socket.setRoutingId(zlink.RoutingId.from(Buffer.from(`multi-router-client-${i}`, 'ascii')));
                socket.options.setConnectRoutingId(SERVER_ROUTING_ID);
            }
            else {
                socket.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${i}`, 'ascii')));
            }
            sockets.push(socket);
            payloads.push(createPayload(options.msgSize));
            pending.push(false);
        }
        for (let i = 0; i < sockets.length; i += 1) {
            await waitForConnectionReady(sockets[i], () => sockets[i].connect(options.endpoint));
            poller.add(sockets[i], pollEvents(POLLOUT), i);
        }
        ctx.recalculateAutoHwm();
        for (const socket of sockets) {
            emitMultiSocketHwmDetail(socket, 'endpoint', options.transport, options.msgSize);
        }
        console.log(`CLIENT_READY,${options.msgSize}`);
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        for await (const line of rl) {
            if (line === `START,${options.msgSize}`) {
                break;
            }
            if (line === 'STOP' || line === 'QUIT') {
                return;
            }
        }
        const runId = createRunId(1);
        const activeStopNs = currentEpochNs()
            + BigInt(Math.floor(options.duration * 1_000_000_000));
        let seq = 1n;
        // The sender owns the active window. Each pass gives every non-pending
        // socket one send, then repeats round-robin until backpressure requires a
        // POLLOUT wakeup. ROUTER clients submit the pass concurrently; DEALER
        // clients keep the pass sequential to avoid flooding the shared ROUTER
        // receive path. Both modes keep all client sockets in the measured flow.
        while (currentEpochNs() < activeStopNs) {
            let pendingCount = 0;
            const sends = [];
            for (let i = 0; i < sockets.length; i += 1) {
                if (pending[i]) {
                    pendingCount += 1;
                    continue;
                }
                if (currentEpochNs() >= activeStopNs) {
                    break;
                }
                stampPayload(payloads[i], {
                    phase: 1,
                    runId,
                    msgSize: options.msgSize,
                    seq
                });
                if (routerClient) {
                    const socketIndex = i;
                    sends.push(sendPayload(sockets[i], routerClient, payloads[i])
                        .then((sent) => ({ socketIndex, sent })));
                }
                else {
                    const sent = await sendPayload(sockets[i], routerClient, payloads[i]);
                    if (!sent) {
                        pending[i] = true;
                        pendingCount += 1;
                    }
                    else {
                        seq += 1n;
                    }
                }
            }
            const results = await Promise.all(sends);
            for (const { socketIndex, sent } of results) {
                if (!sent) {
                    pending[socketIndex] = true;
                    pendingCount += 1;
                }
                else {
                    seq += 1n;
                }
            }
            if (currentEpochNs() >= activeStopNs || pendingCount === 0) {
                continue;
            }
            const readyCount = poller.wait(pollBuffer, -1);
            for (let offset = 0; offset < readyCount; offset += 1) {
                const index = pollBuffer.slot(offset);
                if (!Number.isInteger(index) || index < 0 || index >= sockets.length) {
                    continue;
                }
                if (pollEventHas({ revents: pollBuffer.revents(offset) }, POLLOUT)) {
                    pending[index] = false;
                }
            }
        }
        // Send one wire-level stop token per client socket after the active
        // window. The receiver's tail drain consumes queued payloads and tokens.
        for (const socket of sockets) {
            await sendStopTokenWithRetry(socket, routerClient, poller, pollBuffer);
        }
        console.log(`CLIENT_DONE,${options.msgSize}`);
    }
    finally {
        rl?.close();
        pollBuffer.close();
        poller.close();
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
        poller.add(router, pollEvents(POLLIN), 0);
        pollBuffer = zlink.createPollEvents(1);
        const readyBarrier = waitForConnectionReadyCount(router, options.clients);
        console.log(`READY,${options.endpoint}`);
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        for await (const line of rl) {
            if (line !== `START,${options.msgSize}`) {
                if (line === 'STOP' || line === 'QUIT') {
                    return;
                }
                continue;
            }
            await readyBarrier;
            const payloadSize = Math.max(options.msgSize, HEADER_SIZE);
            const activeStartNs = currentEpochNs();
            const activeStopNs = activeStartNs
                + BigInt(Math.floor(options.duration * 1_000_000_000));
            const collector = createMetricCollector({
                runId: createRunId(1),
                msgSize: options.msgSize,
                activeStartNs,
                activeStopNs,
                latencySampleStride: 32,
                roundTrip: false
            });
            while (currentEpochNs() < activeStopNs) {
                const ready = waitPollerOne(poller, pollBuffer, process.platform === 'win32' ? 50 : -1);
                if (!ready || !pollEventHas(ready, POLLIN)) {
                    continue;
                }
                while (true) {
                    if (!router.recv(received, zlink.RecvFlags.DontWait)) {
                        break;
                    }
                    const data = received.singlePartOrThrow().data();
                    if (!isStopToken(data) && data.length === payloadSize) {
                        collector.recordPayload(data, currentEpochNs());
                    }
                }
            }
            // Drain the in-flight tail and stop tokens without counting it. This
            // keeps the sender's final token from being stranded by receiver HWM.
            const tailReceived = new zlink.Received();
            const tailDeadlineNs = currentEpochNs() + 2000000000n;
            const idleNs = 50000000n;
            let idleDeadlineNs = currentEpochNs() + idleNs;
            while (currentEpochNs() < tailDeadlineNs
                && currentEpochNs() < idleDeadlineNs) {
                let drained = false;
                while (true) {
                    if (!router.recv(tailReceived, zlink.RecvFlags.DontWait)) {
                        break;
                    }
                    drained = true;
                }
                if (drained) {
                    idleDeadlineNs = currentEpochNs() + idleNs;
                    continue;
                }
                const ready = waitPollerOne(poller, pollBuffer, 50);
                if (ready && pollEventHas(ready, POLLIN)) {
                    idleDeadlineNs = currentEpochNs() + idleNs;
                }
            }
            tailReceived.close();
            const result = await collector.finish();
            for (const metricLine of summarizeMetrics(pattern, options.transport, options.msgSize, result.latenciesNs, options.duration, 'current', result.accepted)) {
                console.log(metricLine);
            }
            break;
        }
    }
    finally {
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
    runRoutedSendSendClient,
    runRoutedSendSendServer
};
