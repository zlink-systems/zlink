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
function resolveMultiMonitorHwm(environment = process.env) {
    for (const name of ['PERF_MULTI_MONITOR_HWM', 'PERF_MONITOR_HWM']) {
        const raw = environment[name];
        if (raw === undefined || raw === '') {
            continue;
        }
        const value = Number(raw);
        if (Number.isSafeInteger(value) && value >= 0) {
            return value;
        }
    }
    return 4_096_000;
}
function resolveMultiStreamClientCount(clientCount, transport, environment = process.env) {
    const clients = Math.max(1, Math.trunc(Number(clientCount) || 1));
    if (transport === 'tcp') {
        return clients;
    }
    const configured = Number(environment.PERF_STREAM_NON_TCP_CLIENTS_MAX
        || environment.PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX
        || 10_000);
    const limit = Number.isFinite(configured) && configured > 0
        ? Math.trunc(configured)
        : 10_000;
    return Math.min(clients, limit);
}
module.exports = {
    benchmarkEndpoint,
    parseMultiArgs: parseArgs,
    reservePort,
    resolveMultiConnectConcurrency,
    resolveMultiMonitorHwm,
    resolveMultiStreamClientCount
};
