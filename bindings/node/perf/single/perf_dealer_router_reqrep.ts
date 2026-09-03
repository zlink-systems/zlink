// SPDX-License-Identifier: MPL-2.0
'use strict';
const { summarizeMetrics } = require('../common/perf_metrics');
const { parseSingleBinaryArgs } = require('./perf_single_common');
const { runSocketReqRep } = require('./perf_socket_reqrep');

(async () => {
  const options = parseSingleBinaryArgs(process.argv.slice(2));
  const result = await runSocketReqRep(options.msgSize, options, false);
  if (result.unsupported) {
    console.log(`UNSUPPORTED,${options.libName},DEALER_ROUTER_REQREP,${options.transport}`);
    return;
  }
  for (const line of summarizeMetrics('DEALER_ROUTER_REQREP', options.transport,
    options.msgSize, result.latenciesNs, options.duration, options.libName,
    result.accepted, result.latencyMeanNs)) console.log(line);
})().catch((error) => { console.error(error); process.exitCode = 1; });
