// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const zlink = require('@zlink-systems/zlink');
const { createMetricCollector, createRunId, currentEpochNs, integerEnv, summarizeMetrics, } = require('../common/perf_metrics');
const { applyContextPolicy, applyAutoHwmMsgUnit, applySocketPolicy, benchmarkEndpoint, closeSenderWorker, configureTlsServer, drainRouterRecvInto, emitSingleSocketHwmDetail, parseSingleBinaryArgs, routedLargeMessageSocketPolicy, runLocalSocketOneWayBenchmark, spawnSenderWorker, waitForWorkerError, waitForMonitorConnectionReady, waitForWorkerMessage, } = require('./perf_single_common');
async function runDealerRouterBenchmark(msgSize, options) {
    const socketOptions = routedLargeMessageSocketPolicy(options, msgSize);
    if (options.transport === 'inproc') {
        return runLocalSocketOneWayBenchmark({
            pattern: 'DEALER_ROUTER',
            msgSize,
            options: socketOptions,
            endpointToken: 'dealer-router',
            createReceiver: (ctx) => zlink.createRouterSocket(ctx),
            createSender: (ctx) => zlink.createDealerSocket(ctx),
        });
    }
    const ctx = zlink.createContext();
    applyContextPolicy(ctx);
    const router = zlink.createRouterSocket(ctx);
    const routerMonitor = router.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const endpoint = await benchmarkEndpoint(options.transport, `dealer-router-${msgSize}`);
    let worker = null;
    try {
        applySocketPolicy(router, socketOptions);
        applyAutoHwmMsgUnit(ctx, msgSize);
        ctx.recalculateAutoHwm();
        configureTlsServer(router, options.transport);
        router.bind(endpoint);
        worker = spawnSenderWorker({
            kind: 'dealer_router',
            transport: options.transport,
            endpoint,
            duration: options.duration,
            msgSize,
            runId: options.runId ?? 1,
            options: socketOptions,
        });
        const workerError = waitForWorkerError(worker);
        await Promise.race([
            waitForWorkerMessage(worker, 'ready'),
            workerError.then((message) => Promise.reject(new Error(message.message)))
        ]);
        await waitForMonitorConnectionReady(routerMonitor);
        const activeStartNs = currentEpochNs();
        const activeStopNs = activeStartNs
            + BigInt(Math.floor(options.duration * 1_000_000_000));
        const runId = createRunId(options.runId ?? 1);
        const collector = createMetricCollector({
            runId,
            msgSize,
            activeStartNs,
            activeStopNs,
            latencySampleStride: integerEnv('PERF_SINGLE_ROUTED_LATENCY_SAMPLE_STRIDE', 32),
        });
        // PERF_SINGLE_TEST_POLICY § 1.4 / § 2.0.1: no start/stop control
        // channel. The connection-ready gate above is the only cross-thread
        // sync; the receiver uses blocking recv + drain and exits on the wire
        // stop token (C perf_dealer_router.cpp recv-until-stop-token model).
        const recvTask = drainRouterRecvInto(router, msgSize, Object.assign(collector, { runId, activeStartNs }), { recordUntilNs: activeStopNs });
        await Promise.race([
            recvTask,
            workerError.then((message) => Promise.reject(new Error(message.message)))
        ]);
        const result = collector.finish();
        emitSingleSocketHwmDetail(router, 'DEALER_ROUTER', options.transport, 'receiver', msgSize);
        return result;
    }
    finally {
        await closeSenderWorker(worker);
        routerMonitor.close();
        router.close();
        ctx.close();
    }
}
module.exports = { runDealerRouterBenchmark };
if (require.main === module) {
    (async () => {
        const options = parseSingleBinaryArgs(process.argv.slice(2));
        const result = await runDealerRouterBenchmark(options.msgSize, options);
        for (const line of summarizeMetrics('DEALER_ROUTER', options.transport, options.msgSize, result.latenciesNs, options.duration, options.libName, result.accepted)) {
            console.log(line);
        }
    })().catch((error) => {
        console.error(error);
        process.exitCode = 1;
    });
}
