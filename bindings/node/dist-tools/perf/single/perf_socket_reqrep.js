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
// PERF_SINGLE_TEST_POLICY.md 1.1.3 (D-BP40): the un-settled request set is
// bounded by the socket's own admission window - the SNDHWM bytes Core applied
// to this socket divided by one request's wire size - and never by a fixed
// number. That window is the boundary the C runner observes as
// ZLINK_SUBMIT_BACKPRESSURED (perf_single_reqrep.hpp run_request_phase), so the
// runner keeps no window of its own. A manual PERF_SINGLE_SNDHWM override does
// not appear in the auto-HWM snapshot and is read back from the socket option.
function resolveAdmissionWindow(client, monitor, wireSize) {
    let hwmBytes = 0n;
    try {
        hwmBytes = BigInt(monitor.status().autoHwmAppliedSndHwmBytes ?? 0n);
    }
    catch (error) {
        hwmBytes = 0n;
    }
    if (hwmBytes <= 0n)
        hwmBytes = BigInt(client.options.sendHwm ?? 0n);
    if (hwmBytes <= 0n) {
        throw new Error('requester socket reports no send high-water mark, so the admission '
            + 'window is unknown');
    }
    const window = hwmBytes / BigInt(Math.max(1, wireSize));
    return window > 0n ? Number(window) : 1;
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
    let completionPoller = null;
    let completionEvents = null;
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
        completionPoller = zlink.createPoller();
        completionEvents = zlink.createPollEvents(1);
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
        const payloadTemplate = createPayload(msgSize);
        // `submit()` transfers the request to the binding-owned admission and
        // completion path. Keep its Promise only for settlement and draining.
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
        const admissionWindow = resolveAdmissionWindow(client, clientMonitor, payloadTemplate.length);
        const submitOne = () => {
            const payload = Buffer.from(payloadTemplate);
            stampPayload(payload, { phase: 1, runId, msgSize, seq });
            seq += 1n;
            const task = submitRequest(payload);
            pending.add(task);
            task.catch((error) => { requestFailure = error; })
                .finally(() => pending.delete(task));
        };
        // A turn submits until the un-settled set fills the admission window, then
        // progresses completions on this same thread. Nothing here waits for a
        // reply before submitting the next request, and the window - not a runner
        // constant - is what stops the submit loop.
        while (currentEpochNs() < activeStopNs && !requestFailure) {
            let submittedSinceProgress = 0;
            let submittedAny = false;
            while (currentEpochNs() < activeStopNs && !requestFailure
                && pending.size < admissionWindow) {
                submitOne();
                submittedAny = true;
                // Same progress cadence as the C submit cursor (64 submissions per
                // round). Settling a Promise needs one loop turn on this thread.
                if (++submittedSinceProgress >= 64) {
                    submittedSinceProgress = 0;
                    completionPoller.wait(completionEvents, 0);
                    await sleepImmediate();
                }
            }
            // A bounded wait is reached only when the admission window is full,
            // matching C's blocking poll after backpressure. This thread owns the
            // completion drain, so waiting here is this runner progressing its own
            // requests, not a yield to another scheduler.
            completionPoller.wait(completionEvents, submittedAny ? 0 : 50);
            await sleepImmediate();
        }
        if (requestFailure)
            throw requestFailure;
        // Bounded completion drain of requests submitted before the deadline; the
        // per-request `timeout(requestTimeoutMs)` bounds every one of them and no
        // new request is submitted here.
        const drainStopNs = currentEpochNs()
            + BigInt(Math.max(1_000, requestTimeoutMs * 4)) * 1000000n;
        while (pending.size > 0 && currentEpochNs() < drainStopNs && !requestFailure) {
            completionPoller.wait(completionEvents, 0);
            await sleepImmediate();
        }
        if (pending.size > 0) {
            throw new Error('request completion drain timed out');
        }
        if (requestFailure)
            throw requestFailure;
        const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
        let stopSettled = false;
        let stopFailure = null;
        stopOperation.message(STOP_TOKEN_BYTES).submit()
            .catch((error) => { stopFailure = error; })
            .finally(() => { stopSettled = true; });
        while (!stopSettled) {
            completionPoller.wait(completionEvents, 0);
            await sleepImmediate();
        }
        if (stopFailure)
            throw stopFailure;
        waitForWorkerStatus(worker, 4, 10_000);
        return collector.finish();
    }
    finally {
        await closeSenderWorker(worker);
        try {
            completionPoller?.remove?.(client);
        }
        catch (_) { /* preserve the benchmark failure */ }
        try {
            completionEvents?.close?.();
        }
        catch (_) { /* preserve the benchmark failure */ }
        try {
            completionPoller?.close?.();
        }
        catch (_) { /* preserve the benchmark failure */ }
        for (const resource of [clientMonitor, client, ctx]) {
            try {
                resource?.close?.();
            }
            catch (_) { /* preserve the benchmark failure */ }
        }
    }
}
module.exports = { runSocketReqRep };
