// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { POLLOUT, applyAutoHwmMsgUnit, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, pollEventHas, trySocketSend, waitPollerOne } = require('./perf_multi_runtime');
function packetParts(header, body) {
    const prefix = Buffer.allocUnsafe(6);
    prefix.writeUInt16BE(header.size(), 0);
    prefix.writeUInt32BE(body.size(), 2);
    return [prefix, header, body];
}
function isTransientSendError(error) {
    return error instanceof zlink.SubmitError
        && (error.result === zlink.SubmitResult.Backpressured
            || error.result === zlink.SubmitResult.NotConnected
            || error.result === zlink.SubmitResult.NotFound
            || error.result === zlink.SubmitResult.NotAdmitted);
}
function tryStreamSend(stream, routingId, frame) {
    try {
        return trySocketSend(stream, routingId, frame);
    }
    catch (error) {
        if (isTransientSendError(error)) {
            return false;
        }
        throw error;
    }
}
function pendingLength(pending) {
    return pending.items.length - pending.head;
}
function pushPending(pending, reply) {
    pending.items.push(reply);
}
function closeReply(reply) {
    for (const part of reply.frame) {
        if (part instanceof zlink.Message) {
            part.close();
        }
    }
}
function compactPending(pending) {
    if (pending.head > 0 && pending.head >= pending.items.length) {
        pending.items.length = 0;
        pending.head = 0;
    }
    else if (pending.head > 1024 && pending.head * 2 >= pending.items.length) {
        pending.items.splice(0, pending.head);
        pending.head = 0;
    }
}
function drainPending(stream, pending) {
    while (pending.head < pending.items.length) {
        const reply = pending.items[pending.head];
        if (!tryStreamSend(stream, reply.routingId, reply.frame)) {
            break;
        }
        closeReply(reply);
        pending.head += 1;
    }
    compactPending(pending);
}
function sleepMillis(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'server', 'MULTI_STREAM');
    const stream = zlink.createStreamSocket(ctx);
    const poller = zlink.createPoller();
    let pollBuffer = null;
    let rl = null;
    let stop = false;
    const pending = { items: [], head: 0 };
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
        applyAutoHwmMsgUnit(ctx, options.msgSize);
        ctx.recalculateAutoHwm();
        emitMultiSocketHwmDetail(stream, 'endpoint', options.transport, options.msgSize);
        stream.bind(options.endpoint);
        stream.setPacketHandler((sourceRid, header, body) => {
            let queued = false;
            try {
                const frame = packetParts(header, body);
                if (pendingLength(pending) > 0 || !tryStreamSend(stream, sourceRid, frame)) {
                    pushPending(pending, { routingId: sourceRid, frame });
                    queued = true;
                }
            }
            finally {
                if (!queued) {
                    header.close();
                    body.close();
                }
            }
        });
        poller.add(stream, pollEvents(POLLOUT), 0);
        pollBuffer = zlink.createPollEvents(1);
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
            if (pendingLength(pending) === 0) {
                await sleepMillis(50);
                continue;
            }
            const ready = waitPollerOne(poller, pollBuffer, process.platform === 'win32' ? 50 : -1);
            if (ready && pollEventHas(ready, POLLOUT)) {
                drainPending(stream, pending);
            }
        }
    }
    finally {
        rl?.close();
        for (let index = pending.head; index < pending.items.length; index += 1) {
            closeReply(pending.items[index]);
        }
        pollBuffer?.close();
        poller.close();
        stream.close();
        ctx.close();
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
