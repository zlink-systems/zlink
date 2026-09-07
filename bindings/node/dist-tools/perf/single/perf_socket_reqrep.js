// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, currentEpochNs, sleepImmediate, stampPayload, } = require('../common/perf_metrics');
const { applyContextPolicy, applySocketPolicy, benchmarkEndpoint, closeSenderWorker, configureTlsClient, releaseSenderWorker, spawnSenderWorker, waitForMonitorConnectionReady, waitForWorkerStatus, } = require('./perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
const SERVER_RID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
function closeParts(parts) {
    for (const part of parts ?? [])
        part?.close?.();
}
// Read once per process: the runner fixes PERF_PART_COUNT before launching
// this process, and this is on the per-message path. A per-message
// `process.env` lookup puts harness instrumentation inside the measured
// path and charges it only to the binding runner. C reference:
// bindings/c/perf/common/perf_zlink_part_helpers.hpp
// perf_measurement_part_count.
const MEASUREMENT_PART_COUNT = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
function appendMeasurement(op, payload) {
    op = op.message(payload);
    if (MEASUREMENT_PART_COUNT !== 1)
        op = op.message(Buffer.alloc(0));
    return op;
}
function measurementPayload(parts) {
    const count = MEASUREMENT_PART_COUNT;
    if (!Array.isArray(parts) || parts.length !== count)
        return null;
    if (count === 2 && parts[1].data().length !== 0)
        return null;
    return parts[0];
}
function requestOperation(client, routedClient, payload, timeoutMs) {
    const operation = routedClient ? client.request(SERVER_RID) : client.request();
    return appendMeasurement(operation, payload).timeout(timeoutMs);
}
function routingProbe(client, routedClient, timeoutMs) {
    const expected = Buffer.from('__zlink_perf_reqrep_probe__');
    const parts = requestOperation(client, routedClient, expected, timeoutMs)
        .submit_sync();
    try {
        const payload = measurementPayload(parts);
        return payload !== null && payload.data().equals(expected);
    }
    finally {
        closeParts(parts);
    }
}
async function runSocketReqRep(msgSize, options, routedClient) {
    if (options.transport === 'inproc') {
        // Node Workers cannot share the Context required by inproc.  Keep this
        // explicit in the runner manifest instead of timing out in a second
        // worker with an unreachable endpoint.
        return { unsupported: true };
    }
    const endpoint = await benchmarkEndpoint(options.transport, routedClient ? `router-router-reqrep-${msgSize}` : `dealer-router-reqrep-${msgSize}`);
    const ctx = zlink.createContext();
    applyContextPolicy(ctx);
    const client = routedClient ? zlink.createRouterSocket(ctx)
        : zlink.createDealerSocket(ctx);
    const clientMonitor = client.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
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
        const payloadTemplate = createPayload(msgSize);
        // PERF_SINGLE_TEST_POLICY.md 1.1.3: `submit()` is the awaitable request
        // terminal that merges admission and reply. Do not await it before
        // submitting the next request - keep the un-settled promises in a pending
        // set, drain the ones that settle, and bound only the pending count. This
        // replaces the RTT-only `submit_sync()` loop that PERF_SINGLE_TEST_POLICY.md
        // 1.1.0 forbids because it pinned in-flight to 1.
        const maxOutstanding = Math.max(2, (() => {
            const parsed = Number(process.env.PERF_SINGLE_REQREP_MAX_OUTSTANDING ?? 64);
            return Number.isFinite(parsed) && parsed > 0 ? Math.trunc(parsed) : 64;
        })());
        const pending = new Set();
        let requestFailure = null;
        let seq = 1n;
        const submitRequest = async (payload) => {
            let parts = null;
            try {
                parts = await requestOperation(client, routedClient, payload, requestTimeoutMs)
                    .submit();
                const replyPayload = measurementPayload(parts);
                collector.recordPayload(replyPayload ? replyPayload.data() : null, currentEpochNs());
            }
            catch (error) {
                if (error instanceof zlink.RequestError
                    && error.result === zlink.RequestResult.TimedOut)
                    return;
                throw error;
            }
            finally {
                closeParts(parts);
            }
        };
        while (currentEpochNs() < activeStopNs && !requestFailure) {
            while (pending.size < maxOutstanding && currentEpochNs() < activeStopNs) {
                // Concurrent logical requests cannot share the stamped first part.
                const payload = Buffer.from(payloadTemplate);
                stampPayload(payload, { phase: 1, runId, msgSize, seq });
                seq += 1n;
                const task = submitRequest(payload);
                pending.add(task);
                task.catch((error) => { requestFailure = error; })
                    .finally(() => pending.delete(task));
            }
            await sleepImmediate();
        }
        if (requestFailure)
            throw requestFailure;
        // Bounded completion drain of requests submitted before the deadline; the
        // per-request `timeout(requestTimeoutMs)` bounds every one of them and no
        // new request is submitted here.
        await Promise.all(Array.from(pending));
        if (requestFailure)
            throw requestFailure;
        const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
        stopOperation.message(STOP_TOKEN_BYTES).submit_sync();
        waitForWorkerStatus(worker, 4, 10_000);
        return collector.finish();
    }
    finally {
        await closeSenderWorker(worker);
        for (const resource of [clientMonitor, client, ctx]) {
            try {
                resource?.close?.();
            }
            catch (_) { /* preserve the benchmark failure */ }
        }
    }
}
module.exports = { runSocketReqRep };
