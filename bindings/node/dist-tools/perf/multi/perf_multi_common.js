// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const { MIN_MSG_SIZE } = require('../common/perf_metrics');
const { benchmarkEndpoint: commonBenchmarkEndpoint, reservePort } = require('../common/perf_endpoint');
function parseArgs(argv, defaults = {}) {
    const options = {
        endpoint: '',
        peerEndpoint: '',
        controlEndpoint: '',
        serverControlEndpoint: '',
        transport: 'tcp',
        msgSize: 256,
        duration: 5,
        clients: 1,
        ...defaults
    };
    for (let i = 0; i < argv.length; i += 1) {
        if (argv[i] === '--endpoint') {
            options.endpoint = argv[++i];
        }
        else if (argv[i] === '--peer-endpoint') {
            options.peerEndpoint = argv[++i];
        }
        else if (argv[i] === '--control-endpoint') {
            options.controlEndpoint = argv[++i];
        }
        else if (argv[i] === '--server-control-endpoint') {
            options.serverControlEndpoint = argv[++i];
        }
        else if (argv[i] === '--transport') {
            options.transport = String(argv[++i] || '').trim().toLowerCase();
        }
        else if (argv[i] === '--msg-size') {
            options.msgSize = Number(argv[++i]);
        }
        else if (argv[i] === '--duration') {
            options.duration = Number(argv[++i]);
        }
        else if (argv[i] === '--clients') {
            options.clients = Number(argv[++i]);
        }
    }
    if (!Number.isFinite(options.msgSize) || options.msgSize < MIN_MSG_SIZE) {
        throw new Error(`invalid multi msg size: ${options.msgSize}`);
    }
    if (!Number.isFinite(options.duration) || options.duration <= 0) {
        throw new Error(`invalid multi duration: ${options.duration}`);
    }
    if (!Number.isFinite(options.clients) || options.clients <= 0) {
        throw new Error(`invalid multi clients: ${options.clients}`);
    }
    return options;
}
async function benchmarkEndpoint(transport, token, bindPort = 0) {
    return commonBenchmarkEndpoint(transport, token, { suite: 'multi', bindPort });
}
function resolveMultiConnectConcurrency(clientCount) {
    const configured = Number(process.env.PERF_MULTI_CONNECT_CONCURRENCY);
    if (Number.isFinite(configured) && configured > 0) {
        return Math.trunc(configured);
    }
    return clientCount >= 10000 ? 1024 : 128;
}
module.exports = {
    benchmarkEndpoint,
    parseMultiArgs: parseArgs,
    reservePort,
    resolveMultiConnectConcurrency
};
