// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, HEADER_SIZE, currentEpochNs, sleepImmediate, stampPayload, summarizeMetrics } = require('../common/perf_metrics');
const { configureTlsClient, configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { POLLIN, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, measurementPayload, pollEvents, pollEventHas, recvNoWaitInto, sendRouted, waitForConnectionReady, waitForConnectionReadyCount, waitPollerOne } = require('./perf_multi_runtime');
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
        ? sendRouted(socket, SERVER_ROUTING_ID, payload)
        : sendRouted(socket, payload);
}
async function sendServerReply(router, routingId, parts) {
    try {
        await sendRouted(router, routingId, parts);
        return true;
    }
    catch (error) {
        if (error instanceof zlink.SubmitError
            && (error.result === zlink.SubmitResult.NotConnected
                || error.result === zlink.SubmitResult.NotFound)) {
            return true;
        }
        throw error;
    }
}
async function runRoutedSendSendClient({ options, pattern, routerClient }) {
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'client', pattern);
    const sockets = [];
    const payloads = [];
    const replies = [];
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
            replies.push(new zlink.Received());
        }
        for (let i = 0; i < sockets.length; i += 1) {
            await waitForConnectionReady(sockets[i], () => sockets[i].connect(options.endpoint));
            poller.add(sockets[i], pollEvents(POLLIN), i);
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
        const activeStartNs = currentEpochNs();
        const activeStopNs = activeStartNs
            + BigInt(Math.floor(options.duration * 1_000_000_000));
        const collector = createMetricCollector({
            suite: 'multi',
            runId,
            msgSize: options.msgSize,
            activeStartNs,
            activeStopNs,
            roundTrip: true
        });
        let seq = 1n;
        const sendTasks = sockets.map(async (socket, index) => {
            while (currentEpochNs() < activeStopNs) {
                const currentSeq = seq;
                seq += 1n;
                stampPayload(payloads[index], {
                    phase: 1, runId, msgSize: options.msgSize, seq: currentSeq
                });
                await sendPayload(socket, routerClient, payloads[index]);
                // Inline admission resolves in the microtask queue. Yield to the next
                // event-loop turn so receive readiness and native completions progress
                // concurrently with this socket's next logical send.
                await sleepImmediate();
            }
        });
        let sendsDone = false;
        let sendFailure = null;
        const sendsCompletion = Promise.all(sendTasks).then(() => { sendsDone = true; }, (error) => {
            sendFailure = error;
            sendsDone = true;
        });
        const sendDrainMs = Math.max(1, Number(process.env.PERF_MULTI_SEND_DRAIN_TIMEOUT_MS ?? 1000));
        const sendDrainStopNs = activeStopNs + BigInt(Math.floor(sendDrainMs * 1_000_000));
        while (currentEpochNs() < activeStopNs
            || (!sendsDone && currentEpochNs() < sendDrainStopNs)) {
            if (sendFailure)
                throw sendFailure;
            const nowNs = currentEpochNs();
            const waitStopNs = nowNs < activeStopNs ? activeStopNs : sendDrainStopNs;
            const remainingMs = Math.ceil(Number(waitStopNs - nowNs) / 1_000_000);
            if (remainingMs <= 0) {
                break;
            }
            // The public send Promise is the completion wake for this event-loop
            // turn.  Poll receive readiness without a timer so Promise admission is
            // never advanced by a periodic 1-10 ms progress pump.
            const readyCount = poller.wait(pollBuffer, 0);
            for (let offset = 0; offset < readyCount; offset += 1) {
                const index = pollBuffer.slot(offset);
                if (!Number.isInteger(index) || index < 0 || index >= sockets.length) {
                    continue;
                }
                const event = { revents: pollBuffer.revents(offset) };
                if (pollEventHas(event, POLLIN)) {
                    while (recvNoWaitInto(sockets[index], replies[index])) {
                        const reply = replies[index];
                        try {
                            const payload = measurementPayload(reply.parts);
                            if (!payload)
                                throw new Error('invalid multipart echo reply');
                            collector.recordPayload(payload.data(), currentEpochNs());
                        }
                        finally {
                            reply.close();
                            replies[index] = new zlink.Received();
                        }
                        await sleepImmediate();
                    }
                }
            }
            await sleepImmediate();
        }
        if (!sendsDone) {
            throw new Error('multi routed send admission drain timed out');
        }
        await sendsCompletion;
        if (sendFailure)
            throw sendFailure;
        const result = await collector.finish();
        for (const metricLine of summarizeMetrics(pattern, options.transport, options.msgSize, result.latenciesNs, options.duration, 'current', result.accepted, result.latencyMeanNs)) {
            console.log(metricLine);
        }
        console.log(`CLIENT_DONE,${options.msgSize}`);
    }
    finally {
        rl?.close();
        pollBuffer.close();
        poller.close();
        for (const reply of replies) {
            reply.close();
        }
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
    let received = new zlink.Received();
    const pendingTasks = new Set();
    let sendFailure = null;
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
        let stopRequested = false;
        const started = await new Promise((resolve) => {
            rl.on('line', (line) => {
                if (line === 'STOP' || line === 'QUIT') {
                    stopRequested = true;
                    resolve(false);
                }
                else if (line === `START,${options.msgSize}`) {
                    resolve(true);
                }
            });
        });
        if (!started) {
            return;
        }
        await readyBarrier;
        while (!stopRequested) {
            // Pending public send Promises and stdin both run on this event loop.
            // A zero-time readiness probe followed by setImmediate keeps those
            // signal-driven continuations runnable without a timer pump.
            const ready = waitPollerOne(poller, pollBuffer, 0);
            if (ready && pollEventHas(ready, POLLIN)) {
                while (router.recv(received, zlink.RecvFlags.DontWait)) {
                    try {
                        if (!received.routingId) {
                            throw new Error('routed echo received without routing id');
                        }
                        const expectedParts = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
                        if (received.parts.length !== expectedParts
                            || (expectedParts === 2 && received.parts[1].data().length !== 0)) {
                            const partSizes = received.parts.map((part) => part.data().length).join(',');
                            throw new Error(`invalid multipart echo request: expected=${expectedParts}, sizes=${partSizes}`);
                        }
                        // Snapshot only the application parts before releasing this
                        // receive envelope. routingId remains transport metadata and is
                        // never echoed as an application frame.
                        const task = sendServerReply(router, zlink.RoutingId.from(received.routingId.toBytes()), received.parts.map((part) => Buffer.from(part.data())));
                        pendingTasks.add(task);
                        task.catch((error) => { sendFailure = error; })
                            .finally(() => pendingTasks.delete(task));
                    }
                    finally {
                        received.close();
                        received = new zlink.Received();
                    }
                    await sleepImmediate();
                }
            }
            await sleepImmediate();
            if (sendFailure)
                throw sendFailure;
        }
        await Promise.all(pendingTasks);
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
