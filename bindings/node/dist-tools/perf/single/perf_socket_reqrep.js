// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createPayload, createRunId, currentEpochNs, sleepImmediate, stampPayload, } = require('../common/perf_metrics');
const { applyContextPolicy, applySocketPolicy, benchmarkEndpoint, closeSenderWorker, configureTlsClient, releaseSenderWorker, spawnSenderWorker, waitForMonitorConnectionReady, waitForWorkerStatus, } = require('./perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
const SERVER_RID = zlink.RoutingId.from(Buffer.from('SERVER', 'ascii'));
const COMPLETION_PROGRESS_BATCH = 64;
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
        // Reply completion is independent from admission. Only a backpressured
        // submission pauses this socket's producer loop.
        const pending = new Set();
        let requestFailure = null;
        let seq = 1n;
        let okSinceProgress = 0;
        const collectReply = async (reply) => {
            let parts = null;
            try {
                parts = await reply;
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
        const submitOne = () => {
            const payload = Buffer.from(payloadTemplate);
            stampPayload(payload, { phase: 1, runId, msgSize, seq });
            seq += 1n;
            const submission = requestOperation(client, routedClient, payload, requestTimeoutMs).submit();
            const task = collectReply(submission.reply);
            pending.add(task);
            task.catch((error) => { requestFailure = error; })
                .finally(() => pending.delete(task));
            return submission;
        };
        const waitForAdmission = async (admitted) => {
            let settled = false;
            let failure = null;
            admitted.then(() => { settled = true; }, (error) => { failure = error; settled = true; });
            while (!settled) {
                completionPoller.wait(completionEvents, 50);
                await sleepImmediate();
            }
            if (failure)
                throw failure;
        };
        // Core's result is the only admission window. OK immediately advances to
        // the next request; BACKPRESSURED waits for admission, never for reply.
        while (currentEpochNs() < activeStopNs && !requestFailure) {
            const submission = submitOne();
            if (submission.result === zlink.SubmitResult.Backpressured) {
                await waitForAdmission(submission.admitted);
                okSinceProgress = 0;
            }
            else {
                okSinceProgress += 1;
                if (okSinceProgress === COMPLETION_PROGRESS_BATCH) {
                    okSinceProgress = 0;
                    // C drains this queue after the same bounded OK burst. The public
                    // poller performs that drain before Promise callbacks get a turn.
                    completionPoller.wait(completionEvents, 0);
                    await sleepImmediate();
                }
            }
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
        if (requestFailure)
            throw requestFailure;
        if (pending.size > 0) {
            throw new Error('request completion drain timed out');
        }
        const stopOperation = routedClient ? client.send(SERVER_RID) : client.send();
        const stopSubmission = stopOperation.message(STOP_TOKEN_BYTES).submit();
        if (stopSubmission.result === zlink.SubmitResult.Backpressured) {
            await waitForAdmission(stopSubmission.admitted);
        }
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
