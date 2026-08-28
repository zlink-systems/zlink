// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createRunId, currentEpochNs, HEADER_SIZE, integerEnv, summarizeMetrics } = require('../common/perf_metrics');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { POLLIN, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, pollEventHas, waitPollerOne, waitForConnectionReadyCount } = require('./perf_multi_runtime');
const { isStopToken } = require('../perf_stop_token');
// MULTI_DEALER_DEALER server == RECEIVER / MEASURER.
//
// C parity: bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp
// is the DEALER(bind,1) RECEIVER that runs the deadline-bounded receive
// window (run_receive_window ~208-301) and prints RESULT
// (run_one_size_benchmark ~371-455: throughput = recv_count / duration).
// The matching SENDER is perf_multi_dealer_dealer_client.cpp (N DEALER
// sockets, per-socket stop token ~290-293). Node previously had these
// roles inverted; this file now measures, the client now sends. Cross-
// checked against the already-fixed cpp
// bindings/cpp/perf/multi/src/perf_dealer_dealer_server.cpp ("Measurement
// role: drain incoming payloads"). Handshake (PERF_MULTI § 1.5 / 201):
// server prints READY,<endpoint>; runner spawns client; client prints
// CLIENT_READY,<size>; runner sends START,<size> to BOTH; server runs the
// measured receive window; the per-socket stop tokens wake the `-1`
// poller wait but the duration deadline bounds the phase (C ignores
// expected_stop_count, line 299).
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'server', 'MULTI_DEALER_DEALER');
    const server = zlink.createDealerSocket(ctx);
    const poller = zlink.createPoller();
    const payloadSize = Math.max(options.msgSize, HEADER_SIZE);
    let pollBuffer = null;
    let rl = null;
    let collector = null;
    try {
        applySocketPolicy(server, { transport: options.transport });
        configureTlsServer(server, options.transport);
        server.bind(options.endpoint);
        ctx.recalculateAutoHwm();
        emitMultiSocketHwmDetail(server, 'endpoint', options.transport, options.msgSize);
        poller.add(server, pollEvents(POLLIN), 0);
        pollBuffer = zlink.createPollEvents(1);
        const readyBarrier = waitForConnectionReadyCount(server, options.clients);
        console.log(`READY,${options.endpoint}`);
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        for await (const line of rl) {
            if (line !== `START,${options.msgSize}`) {
                if (line === 'STOP' || line === 'QUIT') {
                    break;
                }
                continue;
            }
            await readyBarrier;
            const activeStartNs = currentEpochNs();
            const activeStopNs = activeStartNs
                + BigInt(Math.floor(options.duration * 1_000_000_000));
            collector = createMetricCollector({
                suite: 'multi',
                runId: createRunId(1),
                msgSize: options.msgSize,
                activeStartNs,
                exactLatencyMean: true,
                latencySampleCap: integerEnv('PERF_MULTI_LATENCY_SAMPLE_CAP', 65536),
            });
            // C run_receive_window (~240-297): poller wait with `-1` (signal-
            // driven), drain DONTWAIT, count active samples while within the
            // measure deadline (the collector enforces the recvTs<=activeStop
            // anchor — perf_measurement.ts ~280), exit once the deadline has
            // elapsed after a drain. Per-socket stop tokens only wake the `-1`
            // wait; the duration deadline bounds the phase (C line 299 ignores
            // the stop count).
            const received = new zlink.Received();
            while (currentEpochNs() < activeStopNs) {
                const ready = waitPollerOne(poller, pollBuffer, process.platform === 'win32' ? 50 : -1);
                if (!ready || !pollEventHas(ready, POLLIN)) {
                    continue;
                }
                while (true) {
                    if (!server.recv(received, zlink.RecvFlags.DontWait)) {
                        break;
                    }
                    const parts = received.parts;
                    const expectedParts = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
                    if (parts.length !== expectedParts || (expectedParts === 2 && parts[1].data().length !== 0)) {
                        received.close();
                        continue;
                    }
                    const data = parts[0].data();
                    const receivedBytes = data.length;
                    if (isStopToken(data)) {
                        continue;
                    }
                    if (receivedBytes === payloadSize) {
                        collector.recordPayload(data, currentEpochNs());
                    }
                }
            }
            // C parity: bindings/c/perf/multi/src/perf_multi_dealer_dealer_server
            // .cpp run_one_size_benchmark (~413-424) drains the tail with
            // drain_phase_until_idle (~303-369, 2.0s max / 50ms idle) AFTER the
            // measure window. This consumes the in-flight payload tail AND the
            // per-socket stop tokens so the SENDER (client)'s blocking per-socket
            // stop-token send completes — without it the sender backpressures
            // forever (server stopped reading) and the runner kills it before
            // the RESULT is collected. The drain does NOT count (window closed).
            const tailMaxNs = 2000000000n;
            const idleNs = 50000000n;
            const tailDeadlineNs = currentEpochNs() + tailMaxNs;
            let idleDeadlineNs = currentEpochNs() + idleNs;
            const tailReceived = new zlink.Received();
            while (currentEpochNs() < tailDeadlineNs && currentEpochNs() < idleDeadlineNs) {
                let drained = false;
                while (true) {
                    if (!server.recv(tailReceived, zlink.RecvFlags.DontWait)) {
                        break;
                    }
                    drained = true;
                }
                if (drained) {
                    idleDeadlineNs = currentEpochNs() + idleNs;
                    continue;
                }
                const ready = waitPollerOne(poller, pollBuffer, 50);
                if (ready && pollEventHas(ready, POLLIN)) {
                    idleDeadlineNs = currentEpochNs() + idleNs;
                }
            }
            const result = await collector.finish();
            for (const resultLine of summarizeMetrics('MULTI_DEALER_DEALER', options.transport, options.msgSize, result.latenciesNs, options.duration, 'current', result.accepted, result.latencyMeanNs)) {
                console.log(resultLine);
            }
            console.log(`CLIENT_DONE,${options.msgSize}`);
            break;
        }
    }
    finally {
        rl?.close();
        pollBuffer?.close();
        poller.close();
        server.close();
        ctx.close();
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
