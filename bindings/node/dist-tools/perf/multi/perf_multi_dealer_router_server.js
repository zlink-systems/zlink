// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { isStopTokenParts } = require('../perf_stop_token');
const { POLLIN, appendMeasurement, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, measurementPayload, waitPollerOne } = require('./perf_multi_runtime');
const { resolveRoutedPattern, runRoutedSendSendServer } = require('./perf_multi_routed_sendsend');
const PATTERN = 'MULTI_DEALER_ROUTER_REQREP';
function receiveAndReply(router, received) {
    while (true) {
        if (!router.recv(received, zlink.RecvFlags.DontWait)) {
            return false;
        }
        try {
            if (!received.routingId) {
                continue;
            }
            const payload = measurementPayload(received.parts);
            if (!payload) {
                received.close();
                continue;
            }
            if (isStopTokenParts([payload])) {
                return true;
            }
            if (received.requestSeq === null) {
                throw new Error('request/reply server received payload without request sequence');
            }
            // A successful public reply consumes this received native Message.
            // Forward it directly rather than materializing a Buffer copy first.
            appendMeasurement(received.reply(), payload).submit();
        }
        finally {
            received.close();
        }
    }
}
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const pattern = resolveRoutedPattern(process.env.PERF_MULTI_PATTERN, 'DEALER_ROUTER');
    if (pattern.endsWith('_SENDSEND')) {
        await runRoutedSendSendServer({
            options,
            pattern,
            family: 'DEALER_ROUTER'
        });
        return;
    }
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'server', PATTERN);
    const router = zlink.createRouterSocket(ctx);
    const poller = zlink.createPoller();
    const received = new zlink.Received();
    let pollBuffer = null;
    let rl = null;
    let stop = false;
    try {
        applySocketPolicy(router);
        configureTlsServer(router, options.transport);
        router.bind(options.endpoint);
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
            const ready = waitPollerOne(poller, pollBuffer, 50);
            await new Promise((resolve) => setImmediate(resolve));
            if (!ready) {
                continue;
            }
            // HOT PATH: waitPollerOne returns the Core revents mask. Test it
            // directly so each ready relay event avoids generic event inspection.
            const revents = ready.revents;
            if ((revents & POLLIN) !== 0) {
                stop = receiveAndReply(router, received);
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
