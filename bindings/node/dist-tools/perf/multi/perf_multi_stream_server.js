// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs, resolveMultiStreamClientCount } = require('./perf_multi_common');
const { applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, waitForConnectionReadyCount, } = require('./perf_multi_runtime');
function packetFrame(header, body) {
    const headerData = header.data();
    const bodyData = body.data();
    const frame = Buffer.allocUnsafe(6 + headerData.length + bodyData.length);
    frame.writeUInt16BE(headerData.length, 0);
    frame.writeUInt32BE(bodyData.length, 2);
    headerData.copy(frame, 6);
    bodyData.copy(frame, 6 + headerData.length);
    return frame;
}
function isGoneRoutingSendError(error) {
    return error instanceof zlink.SubmitError
        && (error.result === zlink.SubmitResult.NotConnected
            || error.result === zlink.SubmitResult.NotFound);
}
async function sendStream(stream, routingId, frame) {
    try {
        // STREAM owns its packet framing; it must remain byte-for-byte unchanged
        // when measurement multipart mode is enabled for the other patterns.
        await stream.send(routingId).message(frame).submit();
    }
    catch (error) {
        if (isGoneRoutingSendError(error)) {
            // The peer route is terminally gone. Treat the reply as handled so the
            // global FIFO can close it and continue instead of pinning every peer
            // behind an entry that can never become writable again.
            return;
        }
        throw error;
    }
}
function sleepImmediate() {
    return new Promise((resolve) => setImmediate(resolve));
}
function streamStartTimeoutMs(environment = process.env) {
    for (const name of [
        'PERF_MULTI_CONNECT_READY_TIMEOUT_MS',
        'PERF_CONNECT_READY_TIMEOUT_MS'
    ]) {
        const value = Number(environment[name]);
        if (Number.isFinite(value) && value > 0) {
            return Math.trunc(value);
        }
    }
    return 10_000;
}
function createStreamControlBarrier(rl, msgSize, timeoutMs = streamStartTimeoutMs()) {
    const expected = `START,${msgSize}`;
    let phase = 'waiting';
    let stopRequested = false;
    let resolveStart;
    let rejectStart;
    const start = new Promise((resolve, reject) => {
        resolveStart = resolve;
        rejectStart = reject;
    });
    const timer = setTimeout(() => {
        if (phase === 'waiting') {
            phase = 'failed';
            rejectStart(new Error(`stream server timeout waiting for ${expected}`));
        }
    }, timeoutMs);
    const onLine = (rawLine) => {
        const line = String(rawLine).trim();
        if (!line) {
            return;
        }
        if (phase === 'waiting') {
            clearTimeout(timer);
            if (line === expected) {
                phase = 'active';
                resolveStart();
            }
            else {
                phase = 'failed';
                rejectStart(new Error(`stream server token mismatch: got ${line}, expected ${expected}`));
            }
            return;
        }
        if (phase === 'active' && (line === 'STOP' || line === 'QUIT')) {
            stopRequested = true;
        }
    };
    rl.on('line', onLine);
    return {
        start,
        stopRequested: () => stopRequested,
        close() {
            clearTimeout(timer);
            rl.off('line', onLine);
        }
    };
}
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'server', 'MULTI_STREAM');
    const stream = zlink.createStreamSocket(ctx);
    let rl = null;
    const pending = new Set();
    let sendFailure = null;
    try {
        applySocketPolicy(stream);
        const bodyMaterialization = (process.env.PERF_STREAM_PACKET_BODY_MATERIALIZATION ?? 'native').toLowerCase();
        if (bodyMaterialization === 'managed') {
            stream.options.packetBodyMaterialization =
                zlink.StreamPacketBodyMaterialization.Managed;
        }
        else if (bodyMaterialization !== 'native') {
            throw new Error('PERF_STREAM_PACKET_BODY_MATERIALIZATION must be native or managed');
        }
        configureTlsServer(stream, options.transport);
        const targetClients = resolveMultiStreamClientCount(options.clients, options.transport);
        let bindCompleted = false;
        let bindError = null;
        const connectionsReady = waitForConnectionReadyCount(stream, targetClients, () => {
            try {
                stream.bind(options.endpoint);
                bindCompleted = true;
            }
            catch (error) {
                bindError = error;
                throw error;
            }
        }, streamStartTimeoutMs()).then(() => null, (error) => error);
        if (!bindCompleted) {
            const connectionError = bindError || await connectionsReady;
            throw connectionError
                || new Error('stream server failed to bind before connection barrier');
        }
        stream.setPacketHandler((sourceRid, header, body) => {
            let reply;
            try {
                reply = {
                    routingId: zlink.RoutingId.from(sourceRid.toBytes()),
                    frame: packetFrame(header, body)
                };
            }
            finally {
                header.close();
                body.close();
            }
            const task = sendStream(stream, reply.routingId, reply.frame);
            pending.add(task);
            task.catch((error) => { sendFailure = error; })
                .finally(() => {
                pending.delete(task);
            });
        });
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        const control = createStreamControlBarrier(rl, options.msgSize);
        console.log(`READY,${options.endpoint}`);
        try {
            await control.start;
            const connectionError = await connectionsReady;
            if (connectionError) {
                throw connectionError;
            }
            ctx.recalculateAutoHwm();
            emitMultiSocketHwmDetail(stream, 'server-connected', options.transport, options.msgSize);
            console.log(`SERVER_START_READY,${options.msgSize}`);
            while (!control.stopRequested()) {
                await sleepImmediate();
                if (sendFailure)
                    throw sendFailure;
            }
        }
        finally {
            control.close();
        }
        await Promise.all(pending);
    }
    finally {
        rl?.close();
        stream.close();
        ctx.close();
    }
}
if (require.main === module) {
    main().catch((error) => {
        console.error(error);
        process.exitCode = 1;
    });
}
module.exports = {
    createStreamControlBarrier,
    streamStartTimeoutMs
};
