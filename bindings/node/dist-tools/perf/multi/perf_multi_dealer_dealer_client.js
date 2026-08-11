// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { currentEpochNs, createPayload, stampPayload, } = require('../common/perf_metrics');
const { configureTlsClient } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { POLLOUT, applyAutoHwmMsgUnit, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, pollEventHas, trySocketSend, waitForConnectionReady } = require('./perf_multi_runtime');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
// MULTI_DEALER_DEALER client == SENDER (one DEALER socket per client).
//
// C parity: bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp
// is the SENDER. It creates one DEALER socket per client (connect),
// prints CLIENT_READY,<size>, waits START,<size> from stdin, runs the
// per-socket DONTWAIT send window (run_send_window ~142-265: send until
// the duration deadline, POLLOUT-wait pending sockets with `-1`), then
// sends exactly ONE blocking wire stop token per socket
// (run_single_size_case ~290-293 / send_stop_token ~114-140). The
// matching RECEIVER/MEASURER is perf_multi_dealer_dealer_server.cpp.
// Cross-checked against the already-fixed cpp
// bindings/cpp/perf/multi/src/perf_dealer_dealer_client.cpp.
// Handshake (PERF_MULTI § 1.5 / line 201): server READY,<endpoint> then
// client spawn; client prints CLIENT_READY,<size>; runner sends
// START,<size> to BOTH; sender runs the send window after START.
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'client', 'MULTI_DEALER_DEALER');
    const dealers = [];
    const poller = zlink.createPoller();
    const pollBuffer = zlink.createPollEvents(Math.max(1, options.clients));
    let rl = null;
    try {
        for (let i = 0; i < options.clients; i += 1) {
            const dealer = zlink.createDealerSocket(ctx);
            applySocketPolicy(dealer, { transport: options.transport });
            configureTlsClient(dealer, options.transport);
            dealers.push(dealer);
        }
        for (let i = 0; i < dealers.length; i += 1) {
            const dealer = dealers[i];
            await waitForConnectionReady(dealer, () => dealer.connect(options.endpoint));
            applyAutoHwmMsgUnit(ctx, options.msgSize);
            poller.add(dealer, pollEvents(POLLOUT), i);
        }
        ctx.recalculateAutoHwm();
        for (const dealer of dealers) {
            emitMultiSocketHwmDetail(dealer, 'endpoint', options.transport, options.msgSize);
        }
        console.log(`CLIENT_READY,${options.msgSize}`);
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        for await (const line of rl) {
            if (line === `START,${options.msgSize}`) {
                break;
            }
            if (line === 'STOP' || line === 'QUIT') {
                return;
            }
        }
        const payloads = dealers.map(() => createPayload(options.msgSize));
        const pending = new Array(dealers.length).fill(false);
        const activeStopNs = currentEpochNs() + BigInt(Math.floor(options.duration * 1_000_000_000));
        let seq = 1n;
        while (currentEpochNs() < activeStopNs) {
            let pendingCount = 0;
            for (let i = 0; i < dealers.length; i += 1) {
                if (pending[i]) {
                    pendingCount += 1;
                    continue;
                }
                // Match the C sender window: keep one writable socket active until
                // backpressure occurs, then wait for POLLOUT before returning to it.
                while (currentEpochNs() < activeStopNs) {
                    stampPayload(payloads[i], { phase: 1, runId: 1, msgSize: options.msgSize, seq });
                    if (!trySocketSend(dealers[i], payloads[i])) {
                        pending[i] = true;
                        pendingCount += 1;
                        break;
                    }
                    seq += 1n;
                }
            }
            if (currentEpochNs() >= activeStopNs || pendingCount === 0) {
                continue;
            }
            const readyCount = poller.wait(pollBuffer, -1);
            for (let offset = 0; offset < readyCount; offset += 1) {
                const index = pollBuffer.slot(offset);
                if (!Number.isInteger(index) || index < 0 || index >= dealers.length) {
                    continue;
                }
                const event = { revents: pollBuffer.revents(offset) };
                if (pollEventHas(event, POLLOUT)) {
                    pending[index] = false;
                }
            }
        }
        for (let i = 0; i < dealers.length; i += 1) {
            const deadline = Date.now() + 5000;
            while (!trySocketSend(dealers[i], STOP_TOKEN_BYTES)) {
                if (Date.now() >= deadline) {
                    throw new Error('stop token send timeout');
                }
                pending[i] = true;
                const readyCount = poller.wait(pollBuffer, 50);
                for (let offset = 0; offset < readyCount; offset += 1) {
                    const index = pollBuffer.slot(offset);
                    if (!Number.isInteger(index) || index < 0 || index >= dealers.length) {
                        continue;
                    }
                    const event = { revents: pollBuffer.revents(offset) };
                    if (pollEventHas(event, POLLOUT)) {
                        pending[index] = false;
                    }
                }
            }
        }
        console.log(`CLIENT_DONE,${options.msgSize}`);
    }
    finally {
        rl?.close();
        pollBuffer.close();
        poller.close();
        for (const dealer of dealers) {
            dealer.close();
        }
        ctx.close();
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
