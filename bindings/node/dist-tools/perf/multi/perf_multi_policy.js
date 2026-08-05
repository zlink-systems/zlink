// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const MULTI_PATTERN_RUNNERS = {
    MULTI_DEALER_DEALER: {
        server: 'perf_multi_dealer_dealer_server.js',
        client: 'perf_multi_dealer_dealer_client.js'
    },
    MULTI_DEALER_ROUTER: {
        server: 'perf_multi_dealer_router_server.js',
        client: 'perf_multi_dealer_router_client.js'
    },
    MULTI_ROUTER_ROUTER: {
        server: 'perf_multi_router_router_server.js',
        client: 'perf_multi_router_router_client.js'
    },
    MULTI_PUBSUB: {
        server: 'perf_multi_pubsub_server.js',
        client: 'perf_multi_pubsub_client.js'
    },
    MULTI_STREAM: {
        server: 'perf_multi_stream_server.js',
        client: null
    }
};
const POLICY_TRANSPORTS = {
    MULTI_DEALER_DEALER: ['tcp', 'tls', 'ws', 'wss'],
    MULTI_DEALER_ROUTER: ['tcp', 'tls', 'ws', 'wss'],
    MULTI_ROUTER_ROUTER: ['tcp', 'tls', 'ws', 'wss'],
    MULTI_PUBSUB: ['tcp', 'tls', 'ws', 'wss'],
    MULTI_STREAM: ['tcp', 'tls', 'ws', 'wss']
};
function patternMsgSizes(patternName, requestedSizes) {
    const allowed = patternName === 'MULTI_STREAM'
        ? [64, 256, 1024, 65536]
        : [64, 256, 1024, 4096, 65536, 131072];
    return requestedSizes.filter((size) => allowed.includes(size));
}
function positiveIntegerEnv(...names) {
    for (const name of names) {
        const value = Number(process.env[name] || NaN);
        if (Number.isFinite(value) && value > 0) {
            return Math.trunc(value);
        }
    }
    return null;
}
function defaultClientsForPattern(patternName) {
    if (patternName === 'MULTI_STREAM') {
        return positiveIntegerEnv('PERF_MULTI_DEFAULT_STREAM_CLIENTS', 'PERF_STREAM_DEFAULT_CLIENTS') ?? 10000;
    }
    return positiveIntegerEnv('PERF_MULTI_DEFAULT_CLIENTS', 'PERF_DEFAULT_CLIENTS') ?? 100;
}
module.exports = {
    MULTI_PATTERN_RUNNERS,
    POLICY_TRANSPORTS,
    patternMsgSizes,
    positiveIntegerEnv,
    defaultClientsForPattern
};
