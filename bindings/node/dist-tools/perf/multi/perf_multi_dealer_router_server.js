// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { isStopToken } = require('../perf_stop_token');
const { POLLIN, POLLOUT, applyAutoHwmMsgUnit, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, pollEventHas, trySocketSend, waitPollerOne } = require('./perf_multi_runtime');
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
            const routingId = received.routingId;
            const payload = received.singlePartOrThrow();
            if (isStopToken(payload.data())) {
                return true;
            }
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
    applyContextPolicy(ctx, 'server', 'MULTI_DEALER_ROUTER');
    const router = zlink.createRouterSocket(ctx);
    const poller = zlink.createPoller();
    const pending = [];
    const received = new zlink.Received();
    let pollBuffer = null;
    let rl = null;
    let stop = false;
    try {
        applySocketPolicy(router);
        configureTlsServer(router, options.transport);
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
            poller.modify(router, pollEvents(POLLIN | POLLOUT));
            const ready = waitPollerOne(poller, pollBuffer, process.platform === 'win32' ? 50 : -1);
            if (!ready) {
                continue;
            }
            if (pollEventHas(ready, POLLOUT)) {
                drainPending(router, pending);
            }
            if (pollEventHas(ready, POLLIN)) {
                stop = receiveAndQueueReplies(router, pending, received);
                drainPending(router, pending);
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
