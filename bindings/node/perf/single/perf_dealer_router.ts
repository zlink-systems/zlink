// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const {
  createMetricCollector,
  createRunId,
  currentEpochNs,
  summarizeMetrics,
} = require('../common/perf_metrics');
const {
  applyContextPolicy,
  applySocketPolicy,
  benchmarkEndpoint,
  closeSenderWorker,
  configureTlsServer,
  drainRouterRecvInto,
  emitSingleSocketHwmDetail,
  parseSingleBinaryArgs,
  runLocalSocketOneWayBenchmark,
  spawnSenderWorker,
  waitForWorkerStatus,
} = require('./perf_single_common');

async function runDealerRouterBenchmark(msgSize, options) {
  if (options.transport === 'inproc') {
    return runLocalSocketOneWayBenchmark({
      pattern: 'DEALER_ROUTER',
      msgSize,
      options,
      endpointToken: 'dealer-router',
      createReceiver: (ctx) => zlink.createRouterSocket(ctx),
      createSender: (ctx) => zlink.createDealerSocket(ctx),
    });
  }

  const ctx = zlink.createContext();
  applyContextPolicy(ctx);
  const router = zlink.createRouterSocket(ctx);
  const endpoint = await benchmarkEndpoint(options.transport, `dealer-router-${msgSize}`);
  let worker = null;

  try {
    applySocketPolicy(router, options);
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
      options,
    });
    waitForWorkerStatus(worker, 3);
    // The worker reports ready only after its CONNECTION_READY monitor has
    // completed. A second ROUTER-side monitor wait requires recv activity to
    // progress and would deadlock before the active receive loop starts.

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
    // channel. The worker connection-ready gate above is the only cross-thread
    // sync; the receiver uses blocking recv + drain and exits on the wire
    // stop token (C perf_dealer_router.cpp recv-until-stop-token model).
    drainRouterRecvInto(
      router,
      msgSize,
      Object.assign(collector, { runId, activeStartNs }),
      { recordUntilNs: activeStopNs }
    );
    waitForWorkerStatus(worker, 4);
    const result = collector.finish();
    emitSingleSocketHwmDetail(router, 'DEALER_ROUTER', options.transport, 'receiver', msgSize);
    return result;
  } finally {
    await closeSenderWorker(worker);
    router.close();
    ctx.close();
  }
}

module.exports = { runDealerRouterBenchmark };

if (require.main === module) {
  (async () => {
    const options = parseSingleBinaryArgs(process.argv.slice(2));
    const result = await runDealerRouterBenchmark(options.msgSize, options);
    if (result.unsupported) {
      console.log(`UNSUPPORTED,${options.libName},DEALER_ROUTER,${options.transport}`);
      return;
    }
    for (const line of summarizeMetrics(
      'DEALER_ROUTER',
      options.transport,
      options.msgSize,
      result.latenciesNs,
      options.duration,
      options.libName,
      result.accepted,
      result.latencyMeanNs
    )) {
      console.log(line);
    }
  })().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
