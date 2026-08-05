// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const { summarizeMetrics } = require('../common/perf_metrics');
const { parseSingleBinaryArgs } = require('./perf_single_common');
const { runSocketReqRep } = require('./perf_socket_reqrep');
(async () => {
    const options = parseSingleBinaryArgs(process.argv.slice(2));
    const result = await runSocketReqRep(options.msgSize, options, true);
    for (const line of summarizeMetrics('ROUTER_ROUTER_REQREP', options.transport, options.msgSize, result.latenciesNs, options.duration, options.libName, result.accepted, 2.0))
        console.log(line);
})().catch((error) => { console.error(error); process.exitCode = 1; });
