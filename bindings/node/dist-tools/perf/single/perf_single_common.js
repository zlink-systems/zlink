// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
exports.configureTlsServer = exports.configureTlsClient = exports.applyAutoHwmMsgUnit = void 0;
exports.applyContextPolicy = applyContextPolicy;
exports.applySocketPolicy = applySocketPolicy;
exports.routedLargeMessageSocketPolicy = routedLargeMessageSocketPolicy;
exports.emitSingleSocketHwmDetail = emitSingleSocketHwmDetail;
exports.benchmarkEndpoint = benchmarkEndpoint;
exports.closeSenderWorker = closeSenderWorker;
exports.drainRouterRecvInto = drainRouterRecvInto;
exports.drainRecvSocket = drainRecvSocket;
exports.parseSingleBinaryArgs = parseSingleBinaryArgs;
exports.runLocalSocketOneWayBenchmark = runLocalSocketOneWayBenchmark;
exports.sendSocketRequired = sendSocketRequired;
exports.spawnSenderWorker = spawnSenderWorker;
exports.waitForWorkerDone = waitForWorkerDone;
exports.waitForWorkerError = waitForWorkerError;
exports.waitForWorkerMessage = waitForWorkerMessage;
exports.waitForPostReadySettle = waitForPostReadySettle;
exports.waitForConnectionReady = waitForConnectionReady;
exports.waitForMonitorConnectionReady = waitForMonitorConnectionReady;
const path = require('node:path');
const { Worker } = require('node:worker_threads');
const zlink = require('@zlink-systems/zlink');
const { MonitorEventType, RecvFlags, RecvResult } = zlink;
const { createMetricCollector, createPayload, createRunId, currentEpochNs, HEADER_SIZE, MIN_MSG_SIZE, applyAutoHwmMsgUnit, applyAutoHwmProfile, integerEnv, manualSocketOverridesEnabled, sleepImmediate, sleepMillis, stampPayload } = require('../common/perf_metrics');
exports.applyAutoHwmMsgUnit = applyAutoHwmMsgUnit;
const { configureTlsClient, configureTlsServer, } = require('../common/perf_tls');
exports.configureTlsClient = configureTlsClient;
exports.configureTlsServer = configureTlsServer;
const { isStopTokenParts } = require('../perf_stop_token');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
const { benchmarkEndpoint: commonBenchmarkEndpoint } = require('../common/perf_endpoint');
const POLLIN = 1;
function pollEvents(mask) {
    const events = [];
    if ((mask & POLLIN) !== 0) {
        events.push(zlink.PollEventFlag.PollIn);
    }
    return events;
}
async function benchmarkEndpoint(transport, token) {
    return commonBenchmarkEndpoint(transport, token, { suite: 'single' });
}
const senderWorkerStates = new WeakMap();
function senderWorkerState(worker) {
    const state = senderWorkerStates.get(worker);
    if (!state) {
        throw new Error('sender worker is not managed by perf_single_common');
    }
    return state;
}
function applySocketPolicy(socket, options = {}) {
    const manualOverrides = manualSocketOverridesEnabled('single');
    const policyOverrides = options.policySocketOverrides === true;
    const hwm = Number.isFinite(options.hwm)
        ? options.hwm
        : integerEnv('PERF_SINGLE_HWM', NaN);
    const sendHwm = Number.isFinite(options.sendHwm)
        ? options.sendHwm
        : integerEnv('PERF_SINGLE_SNDHWM', hwm);
    const recvHwm = Number.isFinite(options.recvHwm)
        ? options.recvHwm
        : integerEnv('PERF_SINGLE_RCVHWM', hwm);
    const sendTimeout = Number.isFinite(options.sendTimeoutMs)
        ? options.sendTimeoutMs
        : integerEnv('PERF_SINGLE_SNDTIMEO_MS', 200);
    const recvTimeout = Number.isFinite(options.recvTimeoutMs)
        ? options.recvTimeoutMs
        : integerEnv('PERF_SINGLE_RCVTIMEO_MS', 200);
    const linger = Number.isFinite(options.lingerMs)
        ? options.lingerMs
        : integerEnv('PERF_SINGLE_LINGER_MS', 0);
    if (socket.options) {
        if (manualOverrides || policyOverrides) {
            if (Number.isFinite(sendHwm) && sendHwm > 0) {
                socket.options.sendHwm = BigInt(sendHwm);
            }
            if (Number.isFinite(recvHwm) && recvHwm > 0) {
                socket.options.recvHwm = BigInt(recvHwm);
            }
        }
        socket.options.sendTimeout = sendTimeout;
        socket.options.recvTimeout = options.recvTimeout ?? recvTimeout;
        socket.options.linger = linger;
        if ('noDrop' in socket.options && options.noDrop !== undefined) {
            socket.options.noDrop = Boolean(options.noDrop);
        }
    }
}
function routedLargeMessageSocketPolicy(options = {}, msgSize) {
    if (manualSocketOverridesEnabled('single')) {
        return options;
    }
    if (!Number.isFinite(msgSize) || msgSize < 65536) {
        return options;
    }
    const defaultFloor = String(options.transport || '').trim().toLowerCase() === 'tcp'
        ? 64
        : 32;
    const floor = integerEnv('PERF_SINGLE_ROUTED_LARGE_HWM_FLOOR', defaultFloor);
    if (!Number.isFinite(floor) || floor <= 0) {
        return options;
    }
    return {
        ...options,
        hwm: Math.max(floor, Number.isFinite(options.hwm) ? Number(options.hwm) : 0),
        policySocketOverrides: true,
    };
}
function socketTypeName(socket) {
    if (typeof socket.setPacketHandler === 'function')
        return 'stream';
    if (typeof socket.reply === 'function')
        return 'router';
    if (typeof socket.request === 'function')
        return 'dealer';
    if (typeof socket.publish === 'function')
        return 'pub';
    if (typeof socket.subscribe === 'function')
        return 'sub';
    if (typeof socket.send === 'function' && typeof socket.recv === 'function')
        return 'pair';
    return 'unknown';
}
function autoHwmRoleName(role) {
    switch (role) {
        case 1: return 'control';
        case 2: return 'routed';
        case 3: return 'fanout';
        case 4: return 'recv_ingress';
        case 5: return 'spot_data';
        case 6: return 'peer_queue';
        case 7: return 'stream';
        default: return 'none';
    }
}
function singleAutoHwmSnapshotVisible(snapshot) {
    return snapshot.autoHwmAppliedSndHwmBytes > 0n
        || snapshot.autoHwmAppliedRcvHwmBytes > 0n
        || BigInt(snapshot.autoHwmEffectiveMessageBytes ?? 0) > 0n
        || BigInt(snapshot.autoHwmSocketMessageSlots ?? 0) > 0n;
}
function emitSingleSocketHwmDetail(socket, pattern, transport, component, msgSize) {
    if (!socket || !pattern || !component) {
        return;
    }
    // Capability gap (e.g. SpotNode has no monitorOpen) is not a fault —
    // simply skip the optional diagnostic for objects that do not expose a
    // monitor surface.
    if (typeof socket.monitorOpen !== 'function') {
        return;
    }
    // PERF_POLICY § 1.1.4: do not silently swallow real failures. A failed
    // `monitorOpen` on a socket that DOES support it means a broken/closed
    // socket — a real benchmark fault that must surface. Only the snapshot
    // read + diagnostic emission itself is best-effort (it never affects
    // the measured RESULT).
    const monitor = socket.monitorOpen([MonitorEventType.ConnectionReady]);
    try {
        const snapshot = monitor.status();
        if (!singleAutoHwmSnapshotVisible(snapshot)) {
            return;
        }
        console.log('AUTO_HWM_DETAIL'
            + `,pattern=${pattern}`
            + `,transport=${transport}`
            + `,component=${component}`
            + `,msg_size=${msgSize}`
            + ',owner=socket'
            + ',owner_id=0'
            + `,socket=${component}`
            + `,socket_type=${socketTypeName(socket)}`
            + `,role=${autoHwmRoleName(snapshot.autoHwmRole)}`
            + `,sndhwm=${snapshot.autoHwmAppliedSndHwmBytes}`
            + `,rcvhwm=${snapshot.autoHwmAppliedRcvHwmBytes}`
            + `,effective_message_bytes=${snapshot.autoHwmEffectiveMessageBytes}`
            + `,effective_sndbuf=${snapshot.autoHwmEffectiveSndBuf}`
            + `,effective_rcvbuf=${snapshot.autoHwmEffectiveRcvBuf}`
            + `,socket_message_slots=${snapshot.autoHwmSocketMessageSlots}`);
    }
    catch (err) {
        // Snapshot/emit is diagnostic only; keep the benchmark result primary.
    }
    finally {
        monitor?.close();
    }
}
function applyContextPolicy(ctx) {
    const ioThreads = integerEnv('PERF_IO_THREADS', 0);
    if (ioThreads > 0) {
        ctx.options.ioThreads = ioThreads;
    }
    const maxSockets = integerEnv('PERF_MAX_SOCKETS', NaN);
    if (Number.isFinite(maxSockets) && maxSockets > 0) {
        ctx.options.maxSockets = maxSockets;
    }
    if ('autoHwmEnabled' in ctx.options) {
        ctx.options.autoHwmEnabled = integerEnv('PERF_CTX_AUTO_HWM_ENABLE', 1) !== 0;
    }
    applyAutoHwmProfile(ctx, zlink);
}
function recvNoWait(socket, received = new zlink.Received(), flags = RecvFlags.DontWait) {
    try {
        return socket.recv(received, flags) ? received : null;
    }
    catch (error) {
        if (error instanceof zlink.RecvError && error.result === RecvResult.NoData) {
            return null;
        }
        throw error;
    }
}
function subscribeNoWait(socket, received = new zlink.TopicMessage(), flags = RecvFlags.DontWait) {
    try {
        return socket.subscribe(received, flags) ? received : null;
    }
    catch (error) {
        if (error instanceof zlink.RecvError && error.result === RecvResult.NoData) {
            return null;
        }
        throw error;
    }
}
function isStopTokenPayload(buffer, size) {
    if (size !== STOP_TOKEN_BYTES.length) {
        return false;
    }
    for (let i = 0; i < STOP_TOKEN_BYTES.length; i += 1) {
        if (buffer[i] !== STOP_TOKEN_BYTES[i]) {
            return false;
        }
    }
    return true;
}
async function waitForConnectionReady(socket, connectFn = null, timeoutMs = integerEnv('PERF_CONNECT_READY_TIMEOUT_MS', 1000)) {
    const monitor = socket.monitorOpen([MonitorEventType.ConnectionReady]);
    try {
        if (typeof connectFn === 'function') {
            await connectFn();
        }
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            try {
                const event = monitor.recv(RecvFlags.DontWait);
                if (event && event.event === MonitorEventType.ConnectionReady) {
                    return;
                }
            }
            catch (error) {
                if (!(error instanceof zlink.RecvError && error.result === RecvResult.NoData)) {
                    throw error;
                }
            }
            await sleepMillis(1);
        }
        throw new Error(`connection ready timeout after ${timeoutMs}ms`);
    }
    finally {
        monitor.close();
    }
}
async function waitForMonitorConnectionReady(monitor, timeoutMs = integerEnv('PERF_CONNECT_READY_TIMEOUT_MS', 1000)) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        try {
            const event = monitor.recv(RecvFlags.DontWait);
            if (event && event.event === MonitorEventType.ConnectionReady) {
                return;
            }
        }
        catch (error) {
            if (!(error instanceof zlink.RecvError && error.result === RecvResult.NoData)) {
                throw error;
            }
        }
        await sleepMillis(1);
    }
    throw new Error(`connection ready timeout after ${timeoutMs}ms`);
}
async function waitForPostReadySettle(timeoutMs) {
    await sleepMillis(Math.max(0, timeoutMs | 0));
}
// C parity: bindings/c/perf/single/common/perf_single_one_way.hpp
// run_active_phase (~269-326) — the receiver thread issues a BLOCKING recv
// bounded by the socket-level `ZLINK_OPT_RCVTIMEO` (set by
// bench_common_runtime.hpp:516 to `PERF_SINGLE_RCVTIMEO_MS`, default 200),
// so recv returns EAGAIN on idle and the loop keeps CYCLING; it only EXITS
// on the wire stop token or a real error (phase end stays wire-stop-token
// driven, PERF_SINGLE_TEST_POLICY § 1.4). C is the sole authority: although
// PERF_SINGLE_TEST_POLICY.md:131 lists the receiver poller wait as `-1`,
// the C reference bounds every individual recv by RCVTIMEO and cycles, so
// we match C and bound the poller wait by the same RCVTIMEO budget rather
// than blocking on `-1`.
//
// This is also load-bearing for correctness: `Poller.wait(events, timeout)` is a
// synchronous N-API call that blocks the JS event loop. An unbounded
// (`-1`) wait blocks the receiver — and the JS loop — forever when the
// peer Worker stalls (observed: a hung DEALER_DEALER/ipc case with the
// main thread parked in `do_sys_poll` and the worker thread futex-blocked
// inside a native zlink call). The `Promise.race` against the sender-Worker
// error channel can then never settle, hard-hanging the case to the
// harness timeout. The RCVTIMEO-bounded wakeup mirrors C's EAGAIN cycle so
// the loop (and the error race) always makes progress, while the
// An occasional `await sleepImmediate()` lets queued Worker postMessages and
// the error channel run without turning every receive cycle into a scheduler
// round trip.
async function drainRecvSocket(socket, onMessage, options = {}) {
    const useSubscribe = typeof socket.subscribe === 'function';
    const recvTimeoutMs = Math.max(1, integerEnv('PERF_SINGLE_RCVTIMEO_MS', 200));
    const recordUntilNs = options.recordUntilNs === undefined
        ? null
        : BigInt(options.recordUntilNs);
    let stopReceived = false;
    let iterCount = 0;
    let totalReceived = 0;
    let recordingActive = true;
    const reusableReceived = useSubscribe
        ? new zlink.TopicMessage()
        : new zlink.Received();
    if (process.env.PERF_NODE_TRACE === '1') {
        console.error(`[drainRecvSocket] entry`);
    }
    while (!stopReceived) {
        iterCount += 1;
        if (process.env.PERF_NODE_TRACE === '1' && (iterCount === 1 || iterCount % 100 === 0)) {
            console.error(`[drainRecvSocket] iter=${iterCount} totalReceived=${totalReceived}`);
        }
        if ((iterCount & 0x3f) === 0) {
            await sleepImmediate();
        }
        let first = true;
        while (true) {
            const received = useSubscribe
                ? subscribeNoWait(socket, reusableReceived, first ? RecvFlags.None : RecvFlags.DontWait)
                : recvNoWait(socket, reusableReceived, first ? RecvFlags.None : RecvFlags.DontWait);
            if (!received) {
                break;
            }
            first = false;
            if (isStopTokenParts(received.parts)) {
                stopReceived = true;
                if (process.env.PERF_NODE_TRACE === '1') {
                    console.error(`[drainRecvSocket] stop totalReceived=${totalReceived}`);
                }
                break;
            }
            totalReceived += 1;
            if (process.env.PERF_NODE_TRACE === '1' && (totalReceived % 100000) === 0) {
                console.error(`[drainRecvSocket] received=${totalReceived}`);
            }
            if (recordUntilNs !== null && recordingActive) {
                recordingActive = currentEpochNs() <= recordUntilNs;
            }
            if (recordUntilNs !== null && !recordingActive) {
                continue;
            }
            onMessage(received);
        }
    }
}
async function drainRouterRecvInto(router, msgSize, onHeader, options = {}) {
    const payloadSize = Math.max(msgSize, HEADER_SIZE);
    const recordUntilNs = options.recordUntilNs === undefined
        ? null
        : BigInt(options.recordUntilNs);
    const metricCollector = typeof onHeader?.recordLatencyNs === 'function'
        ? onHeader
        : null;
    let stopReceived = false;
    let iterCount = 0;
    let totalReceived = 0;
    let recordingActive = true;
    const received = new zlink.Received();
    if (process.env.PERF_NODE_TRACE === '1') {
        console.error(`[drainRouterRecvInto] entry`);
    }
    while (!stopReceived) {
        iterCount += 1;
        if (process.env.PERF_NODE_TRACE === '1' && (iterCount === 1 || iterCount % 100 === 0)) {
            console.error(`[drainRouterRecvInto] iter=${iterCount} totalReceived=${totalReceived}`);
        }
        let first = true;
        while (true) {
            if (!recvNoWait(router, received, first ? RecvFlags.None : RecvFlags.DontWait)) {
                break;
            }
            const data = received.singlePartOrThrow().data();
            const receivedSize = data.length;
            first = false;
            if (isStopTokenPayload(data, receivedSize)) {
                stopReceived = true;
                if (process.env.PERF_NODE_TRACE === '1') {
                    console.error(`[drainRouterRecvInto] stop totalReceived=${totalReceived}`);
                }
                break;
            }
            totalReceived += 1;
            if (process.env.PERF_NODE_TRACE === '1' && (totalReceived % 100000) === 0) {
                console.error(`[drainRouterRecvInto] received=${totalReceived}`);
            }
            const receivedAtNs = currentEpochNs();
            if (recordUntilNs !== null && recordingActive) {
                recordingActive = receivedAtNs <= recordUntilNs;
            }
            if (recordUntilNs !== null && !recordingActive) {
                continue;
            }
            if (metricCollector !== null) {
                metricCollector.recordPayload(data, receivedAtNs);
                continue;
            }
            if (receivedSize !== payloadSize) {
                onHeader(null, receivedAtNs);
                continue;
            }
            onHeader(data, receivedAtNs);
        }
    }
}
function isTransientSubmit(error) {
    const text = String(error && error.message ? error.message : error);
    return (error instanceof zlink.SubmitError
        && (error.result === zlink.SubmitResult.Backpressured
            || error.result === zlink.SubmitResult.NotConnected
            || error.result === zlink.SubmitResult.NotFound))
        || (error && error.code === 'EAGAIN')
        || text.includes('Resource temporarily unavailable')
        || text.includes('Host unreachable')
        || text.includes('Transport endpoint is not connected');
}
function sendSocketNoWait(socket, payload, flags = zlink.SendFlags.DontWait) {
    try {
        return socket.send().message(payload).flags(flags).submit();
    }
    catch (error) {
        if (isTransientSubmit(error)) {
            return false;
        }
        throw error;
    }
}
function sendSocketRequired(socket, payload, flags = zlink.SendFlags.None) {
    socket.send().message(payload).flags(flags).submit();
}
function sendSocketStopWithRetry(socket) {
    for (let retry = 0; retry < 100; retry += 1) {
        try {
            sendSocketRequired(socket, STOP_TOKEN_BYTES);
            return;
        }
        catch (error) {
            if (!isTransientSubmit(error)) {
                throw error;
            }
        }
    }
    throw new Error('stop token send retry budget exhausted');
}
function drainRecvSocketNoWaitUntilIdle(socket, collector, payloadSize, viaSubscribe = false) {
    let stopReceived = false;
    while (true) {
        const received = viaSubscribe ? subscribeNoWait(socket) : recvNoWait(socket);
        if (!received) {
            return stopReceived;
        }
        if (isStopTokenParts(received.parts)) {
            stopReceived = true;
            continue;
        }
        const data = received.singlePartOrThrow().data();
        if (data.length !== payloadSize) {
            collector.recordPayload(null, currentEpochNs());
            continue;
        }
        collector.recordPayload(data, currentEpochNs());
    }
}
async function runLocalSocketOneWayBenchmark({ pattern, msgSize, options, endpointToken, createReceiver, createSender, configureReceiver = null, configureSender = null, 
// Single-context inproc parity extensions (used by PUBSUB / ROUTER_ROUTER
// inproc where the non-inproc path uses a Worker that cannot share the
// inproc context). Defaults preserve the PAIR/DEALER behaviour.
senderBinds = false, // PUBSUB: PUB binds, SUB connects
drainViaSubscribe = false, // PUBSUB: subscriber drains via subscribe
handshake = null, // ROUTER_ROUTER: PING/PONG routing-id gate
sendActive = null, // custom send (publish topic / send rid)
sendStop = null, // custom wire stop-token send
// C parity: the AUTO_HWM_DETAIL `component=` token must match the C
// reference's per-pattern socket labels. PAIR/DEALER use receiver/sender
// (C default); PUBSUB's C reference (perf_pubsub.cpp ~L259-269) labels
// the PUB `publisher` and the SUB `subscriber`, so the inproc path must
// emit those names too (otherwise the `## Auto-HWM Detail` collector
// gains extra non-C `receiver`/`sender` rows for PUBSUB).
receiverHwmComponent = 'receiver', senderHwmComponent = 'sender' }) {
    const ctx = zlink.createContext();
    applyContextPolicy(ctx);
    const receiver = createReceiver(ctx);
    const sender = createSender(ctx);
    const receiverMonitor = receiver.monitorOpen([MonitorEventType.ConnectionReady]);
    const senderMonitor = sender.monitorOpen([MonitorEventType.ConnectionReady]);
    const endpoint = await benchmarkEndpoint(options.transport, `${endpointToken}-${msgSize}`);
    try {
        applySocketPolicy(receiver, options);
        applySocketPolicy(sender, options);
        applyAutoHwmMsgUnit(ctx, msgSize);
        if (typeof configureReceiver === 'function') {
            configureReceiver(receiver);
        }
        if (typeof configureSender === 'function') {
            configureSender(sender);
        }
        ctx.recalculateAutoHwm();
        if (senderBinds) {
            sender.bind(endpoint);
            receiver.connect(endpoint);
        }
        else {
            receiver.bind(endpoint);
            sender.connect(endpoint);
        }
        await waitForMonitorConnectionReady(receiverMonitor);
        await waitForMonitorConnectionReady(senderMonitor);
        // ROUTER_ROUTER routing-id discovery gate (C perf_router_router.cpp
        // does a PING/PONG before the active window). Runs in the single
        // shared context — no Worker, so inproc works.
        let activeRoutingId = null;
        if (typeof handshake === 'function') {
            activeRoutingId = handshake(sender, receiver);
        }
        const activeStartNs = currentEpochNs();
        const activeStopNs = activeStartNs
            + BigInt(Math.floor(options.duration * 1_000_000_000));
        const runId = createRunId(options.runId ?? 1);
        const payload = createPayload(msgSize);
        const payloadSize = Math.max(msgSize, HEADER_SIZE);
        const collector = createMetricCollector({
            runId,
            msgSize,
            activeStartNs,
            activeStopNs,
            latencySampleStride: integerEnv('PERF_SINGLE_LOCAL_LATENCY_SAMPLE_STRIDE', 32),
        });
        // PERF_POLICY § 1.1.2 / C bindings/c/perf/single/common/
        // perf_single_one_way.hpp run_active_phase (~276-358): C runs a
        // dedicated BLOCKING sender thread + a separate receiver thread that
        // share one context. Node Worker threads cannot share a zlink context
        // (inproc is context-local — doc/guide/04-transports.md "Same
        // context"), so the inproc path cannot spawn an OS sender thread the
        // way the non-inproc path uses a sender Worker. The faithful single-
        // context adaptation: emulate C's blocking sender — every message is
        // retried through backpressure until accepted (NO silent EAGAIN drop,
        // NO uncounted send) and the receiver drain between attempts plays the
        // role of C's concurrent receiver thread, relieving backpressure so
        // the send anchor matches C (`sent` advances only on an accepted
        // send, exactly like send_active_samples). The valid-recv / deadline
        // anchor stays in the shared collector (perf_measurement.ts ~280),
        // mirroring C's `steady_clock::now() < deadline` guard.
        const drainOnce = () => drainRecvSocketNoWaitUntilIdle(receiver, collector, payloadSize, drainViaSubscribe);
        const sendOne = typeof sendActive === 'function'
            ? (value) => sendActive(sender, value, activeRoutingId)
            : (value) => sendSocketNoWait(sender, value);
        const sendActiveBlocking = (value) => {
            while (true) {
                if (sendOne(value)) {
                    return true;
                }
                // Backpressured: drain the receiver (the Node stand-in for C's
                // concurrent receiver thread) to relieve HWM, then retry the SAME
                // message. This is the blocking-send semantic, not a drop.
                if (drainOnce()) {
                    // Stop token observed mid-active (should not happen before we
                    // send it) — treat as fatal handshake error rather than a drop.
                    throw new Error('unexpected stop token during active send');
                }
            }
        };
        let seq = 1n;
        while (currentEpochNs() < activeStopNs) {
            stampPayload(payload, { phase: 1, runId, msgSize, seq });
            sendActiveBlocking(payload);
            seq += 1n;
            drainOnce();
        }
        // C single sends active samples until the deadline and then sends only
        // the wire stop token. There is no post-active phase-2 payload.
        if (typeof sendStop === 'function') {
            sendStop(sender, activeRoutingId);
        }
        else {
            sendSocketStopWithRetry(sender);
        }
        while (!drainOnce()) {
            // Drain until the wire-level stop token arrives.
        }
        const result = collector.finish();
        emitSingleSocketHwmDetail(receiver, pattern, options.transport, receiverHwmComponent, msgSize);
        emitSingleSocketHwmDetail(sender, pattern, options.transport, senderHwmComponent, msgSize);
        return result;
    }
    finally {
        receiverMonitor.close();
        senderMonitor.close();
        receiver.close();
        sender.close();
        ctx.close();
    }
}
function parseSingleBinaryArgs(argv) {
    if (argv.length < 3) {
        throw new Error('usage: <binary> <lib_name> <transport> <size>');
    }
    const transport = String(argv[1] || '').trim().toLowerCase();
    const msgSize = Number(argv[2]);
    if (!Number.isFinite(msgSize) || msgSize < MIN_MSG_SIZE) {
        throw new Error(`invalid single msg size: ${argv[2]}`);
    }
    return {
        libName: String(argv[0] || 'current'),
        transport,
        msgSize,
        duration: integerEnv('PERF_SINGLE_DURATION_SECONDS', 5),
        runId: 1,
        hwm: integerEnv('PERF_SINGLE_HWM', NaN),
        sendHwm: integerEnv('PERF_SINGLE_SNDHWM', NaN),
        recvHwm: integerEnv('PERF_SINGLE_RCVHWM', NaN),
        sendTimeoutMs: integerEnv('PERF_SINGLE_SNDTIMEO_MS', NaN),
        recvTimeoutMs: integerEnv('PERF_SINGLE_RCVTIMEO_MS', NaN)
    };
}
function spawnSenderWorker(workerData) {
    const worker = new Worker(path.join(__dirname, 'perf_single_sender_worker.js'), { workerData });
    senderWorkerStates.set(worker, { seenMessages: [], waiters: [] });
    worker.on('message', (message) => {
        const state = senderWorkerState(worker);
        state.seenMessages.push(message);
        for (const waiter of state.waiters.slice()) {
            if (waiter(message)) {
                return;
            }
        }
    });
    return worker;
}
function waitForWorkerMessage(worker, expectedType, timeoutMs = integerEnv('PERF_CONNECT_READY_TIMEOUT_MS', 1000)) {
    return new Promise((resolve, reject) => {
        const state = senderWorkerState(worker);
        const seen = state.seenMessages.find((message) => message && message.type === expectedType);
        if (seen) {
            resolve(seen);
            return;
        }
        let done = false;
        const timeout = setTimeout(() => {
            if (done) {
                return;
            }
            done = true;
            reject(new Error(`worker timeout waiting for ${expectedType}`));
        }, timeoutMs);
        const onExit = (code) => {
            if (done) {
                return;
            }
            done = true;
            clearTimeout(timeout);
            reject(new Error(`worker exited before ${expectedType}: ${code}`));
        };
        worker.once('exit', onExit);
        state.waiters.push((message) => {
            if (done || !message || message.type !== expectedType) {
                return false;
            }
            done = true;
            clearTimeout(timeout);
            worker.off('exit', onExit);
            resolve(message);
            return true;
        });
    });
}
function waitForWorkerError(worker) {
    return new Promise((resolve) => {
        const state = senderWorkerState(worker);
        const seen = state.seenMessages.find((message) => message && message.type === 'error');
        if (seen) {
            resolve(seen);
            return;
        }
        state.waiters.push((message) => {
            if (!message || message.type !== 'error') {
                return false;
            }
            resolve(message);
            return true;
        });
    });
}
function waitForWorkerDone(worker, durationSeconds) {
    const readyTimeoutMs = integerEnv('PERF_CONNECT_READY_TIMEOUT_MS', 1000);
    const activeMs = Math.ceil(Math.max(0, Number(durationSeconds) || 0) * 1000);
    return waitForWorkerMessage(worker, 'done', activeMs + readyTimeoutMs);
}
async function closeSenderWorker(worker) {
    if (!worker) {
        return;
    }
    const waitForExit = new Promise((resolve) => {
        worker.once('exit', () => resolve());
    });
    try {
        worker.postMessage({ type: 'stop' });
    }
    catch (err) {
        console.error(`[perf] close failed: ${err}`);
    }
    const exited = await Promise.race([
        waitForExit.then(() => true),
        new Promise((resolve) => setTimeout(() => resolve(false), 1000))
    ]);
    if (!exited && worker.threadId && Number.isFinite(worker.threadId)) {
        try {
            await worker.terminate();
        }
        catch (err) {
            console.error(`[perf] close failed: ${err}`);
        }
    }
}
module.exports = {
    applyAutoHwmMsgUnit,
    applyContextPolicy,
    applySocketPolicy,
    routedLargeMessageSocketPolicy,
    configureTlsClient,
    configureTlsServer,
    emitSingleSocketHwmDetail,
    benchmarkEndpoint,
    closeSenderWorker,
    drainRouterRecvInto,
    drainRecvSocket,
    parseSingleBinaryArgs,
    runLocalSocketOneWayBenchmark,
    sendSocketRequired,
    spawnSenderWorker,
    waitForWorkerDone,
    waitForWorkerError,
    waitForWorkerMessage,
    waitForPostReadySettle,
    waitForConnectionReady,
    waitForMonitorConnectionReady,
};
