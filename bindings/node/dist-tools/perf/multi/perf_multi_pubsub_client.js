// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createRunId, currentEpochNs, HEADER_SIZE, summarizeMetrics } = require('../common/perf_metrics');
const { integerEnv } = require('../common/perf_args');
const { configureTlsClient } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const { POLLIN, applyContextPolicy, applySocketPolicy, emitMultiSocketHwmDetail, pollEvents, pollEventHas, waitForConnectionReady } = require('./perf_multi_runtime');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
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
async function main() {
    const options = parseMultiArgs(process.argv.slice(2));
    const ctx = zlink.createContext();
    applyContextPolicy(ctx, 'client', 'MULTI_PUBSUB');
    const subs = [];
    const receivedBySub = [];
    let rl = null;
    let collector = null;
    let poller = null;
    let pollBuffer = null;
    try {
        for (let i = 0; i < options.clients; i += 1) {
            const sub = zlink.createSubSocket(ctx);
            applySocketPolicy(sub);
            configureTlsClient(sub, options.transport);
            sub.setSubscription('');
            await waitForConnectionReady(sub, () => sub.connect(options.endpoint));
            subs.push(sub);
            receivedBySub.push(new zlink.TopicMessage());
        }
        ctx.recalculateAutoHwm();
        for (const sub of subs) {
            emitMultiSocketHwmDetail(sub, 'endpoint', options.transport, options.msgSize);
        }
        // The C harness prepares its poll items before the active window starts.
        // Keep the same boundary here so setting up 100 subscriptions is never
        // charged to the receive measurement.
        poller = zlink.createPoller();
        pollBuffer = zlink.createPollEvents(Math.max(1, subs.length));
        for (let i = 0; i < subs.length; i += 1) {
            poller.add(subs[i], pollEvents(POLLIN), i);
        }
        console.log(`CLIENT_READY,${options.msgSize}`);
        rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
        for await (const line of rl) {
            if (line === `START,${options.msgSize}`) {
                const activeStartNs = currentEpochNs();
                const activeStopNs = activeStartNs + BigInt(Math.floor(options.duration * 1_000_000_000));
                collector = createMetricCollector({
                    runId: createRunId(1),
                    msgSize: options.msgSize,
                    activeStartNs,
                    activeStopNs,
                    latencySampleStride: integerEnv('PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE', 32),
                });
                // C parity: run_recv_duration checks the active deadline before
                // each poll and waits no more than 100ms. A received socket is then
                // drained with DONT_WAIT until empty. The wire stop token still
                // ends the phase when it arrives first.
                let stopReceived = false;
                while (!stopReceived) {
                    const remainingNs = activeStopNs - BigInt(currentEpochNs());
                    if (remainingNs <= 0n) {
                        break;
                    }
                    const waitMs = Number((remainingNs + 999999n) / 1000000n);
                    const readyCount = poller.wait(pollBuffer, Math.min(100, Math.max(1, waitMs)));
                    if (readyCount === 0) {
                        continue;
                    }
                    for (let offset = 0; offset < readyCount; offset += 1) {
                        const index = pollBuffer.slot(offset);
                        if (!Number.isInteger(index) || index < 0 || index >= subs.length
                            || !pollEventHas({ revents: pollBuffer.revents(offset) }, POLLIN)) {
                            continue;
                        }
                        while (true) {
                            if (BigInt(currentEpochNs()) >= activeStopNs) {
                                stopReceived = true;
                                break;
                            }
                            const received = receivedBySub[index];
                            if (!subs[index].subscribe(received, zlink.RecvFlags.DontWait)) {
                                break;
                            }
                            const data = received.singlePartOrThrow().data();
                            if (isStopTokenPayload(data, data.length)) {
                                stopReceived = true;
                                continue;
                            }
                            collector.recordPayload(data, currentEpochNs());
                        }
                    }
                }
                break;
            }
        }
        const result = collector ? await collector.finish() : { latenciesNs: [] };
        for (const line of summarizeMetrics('MULTI_PUBSUB', options.transport, options.msgSize, result.latenciesNs, options.duration, 'current', result.accepted)) {
            console.log(line);
        }
        console.log(`CLIENT_DONE,${options.msgSize}`);
    }
    finally {
        rl?.close();
        pollBuffer?.close();
        poller?.close();
        for (const received of receivedBySub) {
            received.close();
        }
        for (const sub of subs) {
            sub.close();
        }
        ctx.close();
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
