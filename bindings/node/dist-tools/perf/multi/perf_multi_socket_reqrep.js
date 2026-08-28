// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, currentEpochNs, sleepImmediate, stampPayload, summarizeMetrics } = require('../common/perf_metrics');
const { configureTlsClient } = require('../common/perf_tls');
const { applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, waitForConnectionReady } = require('./perf_multi_runtime');
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
            if (routerClient)
                socket.options.setConnectRoutingId(serverRoutingId);
            sockets.push(socket);
            await waitForConnectionReady(socket, () => socket.connect(options.endpoint));
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
            activeStartNs, activeStopNs, roundTrip: true
        });
        const requestTimeoutMs = Math.max(1, Number(process.env.PERF_MULTI_REQREP_TIMEOUT_MS ?? 200));
        let seq = 1n;
        const payloadTemplates = sockets.map(() => createPayload(options.msgSize));
        const retryPayloads = sockets.map(() => []);
        const pending = new Set();
        let requestFailure = null;
        const submitRequest = async (socket, payload) => {
            let parts = null;
            try {
                const operation = routerClient ? socket.request(serverRoutingId) : socket.request();
                parts = await appendMeasurement(operation, payload)
                    .timeout(requestTimeoutMs).submit();
                const replyPayload = measurementPayload(parts);
                collector.recordPayload(replyPayload?.data?.() ?? null, currentEpochNs());
            }
            catch (error) {
                if (error instanceof zlink.SubmitError
                    && (error.result === zlink.SubmitResult.Backpressured
                        || error.result === zlink.SubmitResult.NotAdmitted))
                    return true;
                if (error instanceof zlink.RequestError
                    && error.result === zlink.RequestResult.TimedOut)
                    return false;
                throw error;
            }
            finally {
                closeParts(parts);
            }
            return false;
        };
        while (currentEpochNs() < activeStopNs) {
            for (let index = 0; index < sockets.length; index += 1) {
                let payload = retryPayloads[index].shift();
                if (!payload) {
                    payload = Buffer.from(payloadTemplates[index]);
                    const currentSeq = seq;
                    seq += 1n;
                    stampPayload(payload, {
                        phase: 1, runId, msgSize: options.msgSize, seq: currentSeq
                    });
                }
                const logicalPayload = payload;
                const task = submitRequest(sockets[index], logicalPayload).then((retry) => {
                    if (retry && currentEpochNs() < activeStopNs) {
                        // Preserve every pre-admission failure as its exact logical
                        // request. Queued retries take priority over allocating another
                        // sequence on a later event-loop turn.
                        retryPayloads[index].push(logicalPayload);
                    }
                });
                pending.add(task);
                task.catch((error) => { requestFailure = error; })
                    .finally(() => pending.delete(task));
            }
            await sleepImmediate();
            if (requestFailure)
                throw requestFailure;
        }
        await Promise.all(Array.from(pending));
        if (requestFailure)
            throw requestFailure;
        const result = await collector.finish();
        for (const line of summarizeMetrics(pattern, options.transport, options.msgSize, result.latenciesNs, options.duration, 'current', result.accepted, result.latencyMeanNs)) {
            console.log(line);
        }
        console.log(`CLIENT_DONE,${options.msgSize}`);
    }
    finally {
        rl?.close();
        for (const socket of sockets)
            socket.close();
        ctx.close();
    }
}
module.exports = { runSocketReqRepClient };
