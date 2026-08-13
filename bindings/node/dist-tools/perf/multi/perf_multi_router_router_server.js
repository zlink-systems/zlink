// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { isStopTokenParts } = require('../perf_stop_token');
const { POLLIN, POLLOUT, applyAutoHwmMsgUnit, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, trySocketSend, waitPollerOne } = require('./perf_multi_runtime');
function drainPending(router, pending) {
    while (pending.length > 0) {
        const reply = pending[0];
        if (!trySocketSend(router, reply.routingId, reply.payload)) {
            break;
        }
        pending.shift();
    }
}
function receiveAndQueueReplies(router, pending, received) {
    while (true) {
        if (!router.recv(received, zlink.RecvFlags.DontWait)) {
            return false;
        }
        try {
            if (!received.routingId || received.requestSeq) {
                continue;
            }
            const payload = received.singlePartOrThrow();
            if (isStopTokenParts([payload])) {
                return true;
            }
            const routingId = received.routingId;
            if (pending.length === 0 && trySocketSend(router, routingId, payload)) {
                continue;
            }
            pending.push({ routingId, payload: Buffer.from(payload.data()) });
        }
        finally {
            received.close();
        }
    }
}
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'server', 'MULTI_ROUTER_ROUTER');
    const router = zlink.createRouterSocket(ctx);
    const poller = zlink.createPoller();
    const pending = [];
    const received = new zlink.Received();
    let pollBuffer = null;
    let rl = null;
    let stop = false;
    let pollMask = POLLIN;
    try {
        applySocketPolicy(router);
        configureTlsServer(router, options.transport);
        router.setRoutingId(zlink.RoutingId.from(Buffer.from('SERVER', 'ascii')));
        router.bind(options.endpoint);
        applyAutoHwmMsgUnit(ctx, options.msgSize);
        ctx.recalculateAutoHwm();
        emitMultiSocketHwmDetail(router, 'endpoint', options.transport, options.msgSize);
        poller.add(router, pollEvents(POLLIN), 0);
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
            const ready = waitPollerOne(poller, pollBuffer, process.platform === 'win32' ? 50 : -1);
            if (!ready) {
                continue;
            }
            // HOT PATH: waitPollerOne returns the Core revents mask. Test it
            // directly so each ready relay event avoids generic event inspection.
            const revents = ready.revents;
            if ((revents & POLLOUT) !== 0) {
                drainPending(router, pending);
            }
            if ((revents & POLLIN) !== 0) {
                stop = receiveAndQueueReplies(router, pending, received);
                drainPending(router, pending);
            }
            const nextPollMask = pending.length > 0 ? POLLIN | POLLOUT : POLLIN;
            if (nextPollMask !== pollMask) {
                // HOT PATH: keep the relay asleep on POLLIN unless a queued reply
                // needs a writable notification. This is the C relay poll contract;
                // permanent POLLOUT interest makes an idle ROUTER spin.
                poller.modify(router, pollEvents(nextPollMask));
                pollMask = nextPollMask;
            }
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
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
