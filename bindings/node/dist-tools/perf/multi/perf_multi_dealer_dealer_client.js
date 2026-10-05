// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { currentEpochNs, createPayload, sleepImmediate, stampPayload, } = require('../common/perf_metrics');
const { configureTlsClient } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { POLLCOMPLETION, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, measurementParts, pollEvents, sendRouted, waitForConnectionReady, waitPollerOne } = require('./perf_multi_runtime');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
// DEALER_DEALER client == SENDER (one DEALER socket per client).
//
// C parity: bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp
// is the SENDER. It creates one DEALER socket per client (connect),
// prints CLIENT_READY,<size>, waits START,<size> from stdin, runs the
// per-socket bounded send window (run_send_window ~142-265). This binding
// awaits the event-loop-managed WRITABLE retry terminal instead of exposing a
// DONTWAIT loop to the benchmark,
// then sends exactly ONE wire stop token per socket
// (run_single_size_case ~290-293 / send_stop_token ~114-140). The
// matching RECEIVER/MEASURER is perf_multi_dealer_dealer_server.cpp.
// Cross-checked against the already-fixed cpp
// bindings/cpp/perf/multi/src/perf_dealer_dealer_client.cpp.
// Handshake (PERF_MULTI § 1.5 / line 201): server READY,<endpoint> then
// client spawn; client prints CLIENT_READY,<size>; runner sends
// START,<size> to BOTH; sender runs the send window after START.
async function runDealerDealerSendRounds({ dealers, payloads, msgSize, activeStopNs, runId = 1, maxTurns = Number.POSITIVE_INFINITY, submit = sendRouted, drainCompletions = null, yieldTurn = sleepImmediate }) {
    let seq = 1n;
    let turns = 0;
    let nextSocket = 0;
    let failure = null;
    const available = dealers.map(() => true);
    const blocked = new Map();
    // Each socket owns one stable JS measurement record. The first Buffer is
    // stamped only while that socket has no admission in flight; the empty
    // second part is the process-wide immutable-length tail.
    const records = payloads.map((payload) => measurementParts(payload));
    const submitOne = (index) => {
        available[index] = false;
        let submission;
        try {
            submission = submit(dealers[index], records[index]);
        }
        catch (error) {
            failure = error;
            return;
        }
        if (submission.result === zlink.SubmitResult.Ok) {
            available[index] = true;
            return;
        }
        const admission = submission.admitted.then(() => { available[index] = true; }, (error) => { failure = error; }).finally(() => blocked.delete(index));
        blocked.set(index, admission);
    };
    while (turns < maxTurns && currentEpochNs() < activeStopNs) {
        for (let offset = 0; offset < dealers.length; offset += 1) {
            if (currentEpochNs() >= activeStopNs)
                break;
            const index = (nextSocket + offset) % dealers.length;
            if (!available[index])
                continue;
            const currentSeq = seq;
            seq += 1n;
            stampPayload(payloads[index], {
                phase: 1, runId, msgSize, seq: currentSeq
            });
            // Immediate admission keeps this socket runnable. Only a backpressured
            // socket leaves the round-robin set until its admission stage resolves.
            submitOne(index);
            if (failure)
                throw failure;
        }
        nextSocket = (nextSocket + 1) % dealers.length;
        turns += 1;
        if (blocked.size === dealers.length) {
            if (drainCompletions) {
                drainCompletions(50);
                await yieldTurn();
            }
            else {
                await Promise.race(blocked.values());
            }
        }
        else if (maxTurns !== Number.POSITIVE_INFINITY) {
            await yieldTurn();
        }
        else {
            drainCompletions?.(0);
        }
        if (failure)
            throw failure;
    }
    // The active deadline stops new payloads. Finish managed admissions that the
    // binding may still be retrying before emitting each wire-level stop token.
    while (blocked.size > 0) {
        if (drainCompletions) {
            drainCompletions(50);
            await yieldTurn();
        }
        else {
            await Promise.race(blocked.values());
        }
        if (failure)
            throw failure;
    }
    return { turns, sent: seq - 1n };
}
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'client', 'DEALER_DEALER');
    const dealers = [];
    const poller = zlink.createPoller();
    const pollEventsBuffer = zlink.createPollEvents(Math.max(1, options.clients));
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
            poller.add(dealer, pollEvents(POLLCOMPLETION), i);
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
        const activeStopNs = currentEpochNs() + BigInt(Math.floor(options.duration * 1_000_000_000));
        await runDealerDealerSendRounds({
            dealers,
            payloads,
            msgSize: options.msgSize,
            activeStopNs,
            drainCompletions: (timeoutMs) => waitPollerOne(poller, pollEventsBuffer, timeoutMs)
        });
        const stopAdmissions = dealers.map((dealer) => sendRouted(dealer, [STOP_TOKEN_BYTES]))
            .filter((submission) => submission.result === zlink.SubmitResult.Backpressured)
            .map((submission) => submission.admitted);
        let pendingStops = stopAdmissions.length;
        let stopFailure = null;
        const stopDrain = Promise.all(stopAdmissions.map((admission) => admission.then(() => { pendingStops -= 1; }, (error) => { stopFailure ??= error; pendingStops -= 1; })));
        while (pendingStops > 0 && !stopFailure) {
            waitPollerOne(poller, pollEventsBuffer, 50);
            await sleepImmediate();
        }
        if (!stopFailure)
            await stopDrain;
        if (stopFailure)
            throw stopFailure;
        console.log(`CLIENT_DONE,${options.msgSize}`);
    }
    finally {
        rl?.close();
        pollEventsBuffer.close();
        poller.close();
        for (const dealer of dealers) {
            dealer.close();
        }
        ctx.close();
    }
}
module.exports = { runDealerDealerSendRounds };
if (require.main === module) {
    main().catch((error) => {
        console.error(error);
        process.exitCode = 1;
    });
}
