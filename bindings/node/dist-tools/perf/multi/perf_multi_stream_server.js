// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, } = require('./perf_multi_runtime');
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
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'server', 'MULTI_STREAM');
    const stream = zlink.createStreamSocket(ctx);
    let rl = null;
    let stop = false;
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
        stream.bind(options.endpoint);
        ctx.recalculateAutoHwm();
        emitMultiSocketHwmDetail(stream, 'endpoint', options.transport, options.msgSize);
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
        console.log(`READY,${options.endpoint}`);
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        (async () => {
            for await (const line of rl) {
                if (line === 'STOP' || line === 'QUIT') {
                    stop = true;
                    break;
                }
            }
        })();
        while (!stop) {
            await sleepImmediate();
            if (sendFailure)
                throw sendFailure;
        }
        await Promise.all(pending);
    }
    finally {
        rl?.close();
        stream.close();
        ctx.close();
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
