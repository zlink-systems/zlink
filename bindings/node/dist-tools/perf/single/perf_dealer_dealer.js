// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createRunId, currentEpochNs, HEADER_SIZE, summarizeMetrics, } = require('../common/perf_metrics');
const { applyContextPolicy, applySocketPolicy, benchmarkEndpoint, closeSenderWorker, configureTlsServer, drainRecvSocket, emitSingleSocketHwmDetail, measurementPayload, parseSingleBinaryArgs, runLocalSocketOneWayBenchmark, spawnSenderWorker, waitForWorkerError, waitForMonitorConnectionReady, waitForWorkerMessage, } = require('./perf_single_common');
async function runDealerDealerBenchmark(msgSize, options) {
    if (options.transport === 'inproc') {
        return runLocalSocketOneWayBenchmark({
            pattern: 'DEALER_DEALER',
            msgSize,
            options,
            endpointToken: 'dealer-dealer',
            createReceiver: (ctx) => zlink.createDealerSocket(ctx),
            createSender: (ctx) => zlink.createDealerSocket(ctx),
        });
    }
    const ctx = zlink.createContext();
    applyContextPolicy(ctx);
    const server = zlink.createDealerSocket(ctx);
    const serverMonitor = server.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const endpoint = await benchmarkEndpoint(options.transport, `dealer-dealer-${msgSize}`);
    let worker = null;
    try {
        applySocketPolicy(server, options);
        ctx.recalculateAutoHwm();
        configureTlsServer(server, options.transport);
        server.bind(endpoint);
        worker = spawnSenderWorker({
            kind: 'dealer_dealer',
            transport: options.transport,
            endpoint,
            duration: options.duration,
            msgSize,
            runId: options.runId ?? 1,
            options,
        });
        const workerError = waitForWorkerError(worker);
        await Promise.race([
            waitForWorkerMessage(worker, 'ready'),
            workerError.then((message) => Promise.reject(new Error(message.message)))
        ]);
        await waitForMonitorConnectionReady(serverMonitor);
        const activeStartNs = currentEpochNs();
        const activeStopNs = activeStartNs
            + BigInt(Math.floor(options.duration * 1_000_000_000));
        const runId = createRunId(options.runId ?? 1);
        const collector = createMetricCollector({
            runId,
            msgSize,
            activeStartNs,
            activeStopNs,
        });
        // PERF_SINGLE_TEST_POLICY § 1.4 / § 2.0.1: no start/stop control
        // channel. The connection-ready gate above is the only cross-thread
        // sync (mirrors C spawning sender/receiver threads after
        // setup_connected_*); the receiver drains until the wire stop token,
        // exactly like C perf_single_one_way.hpp run_active_phase.
        const recvTask = drainRecvSocket(server, (received) => {
            const payload = measurementPayload(received.parts);
            if (!payload) {
                collector.recordPayload(null, currentEpochNs());
                return;
            }
            const data = payload.data();
            if (data.length !== Math.max(msgSize, HEADER_SIZE)) {
                collector.recordPayload(null, currentEpochNs());
                return;
            }
            collector.recordPayload(data, currentEpochNs());
        }, { recordUntilNs: activeStopNs });
        await Promise.race([
            recvTask,
            workerError.then((message) => Promise.reject(new Error(message.message)))
        ]);
        const result = collector.finish();
        emitSingleSocketHwmDetail(server, 'DEALER_DEALER', options.transport, 'receiver', msgSize);
        return result;
    }
    finally {
        await closeSenderWorker(worker);
        serverMonitor.close();
        server.close();
        ctx.close();
    }
}
module.exports = { runDealerDealerBenchmark };
if (require.main === module) {
    (async () => {
        const options = parseSingleBinaryArgs(process.argv.slice(2));
        const result = await runDealerDealerBenchmark(options.msgSize, options);
        if (result.unsupported) {
            console.log(`UNSUPPORTED,${options.libName},DEALER_DEALER,${options.transport}`);
            return;
        }
        for (const line of summarizeMetrics('DEALER_DEALER', options.transport, options.msgSize, result.latenciesNs, options.duration, options.libName, result.accepted, result.latencyMeanNs)) {
            console.log(line);
        }
    })().catch((error) => {
        console.error(error);
        process.exitCode = 1;
    });
}
