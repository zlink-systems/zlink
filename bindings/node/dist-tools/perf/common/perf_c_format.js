// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
function resolveLatencyTriplet(latency, latencyP95, latencyP99) {
    const mean = latency;
    let p95 = latencyP95 !== undefined && latencyP95 !== null ? latencyP95 : mean;
    if (p95 === undefined || p95 === null) {
        p95 = mean;
    }
    let p99 = latencyP99 !== undefined && latencyP99 !== null ? latencyP99 : p95;
    if (p99 === undefined || p99 === null) {
        p99 = (p95 !== undefined && p95 !== null) ? p95 : mean;
    }
    return [mean, p95, p99];
}
function fixed(value, decimals, width) {
    const text = Number(value).toFixed(decimals);
    return width > 0 ? text.padStart(width) : text;
}
function padStart(value, width) {
    return String(value).padStart(width);
}
function padEnd(value, width) {
    return String(value).padEnd(width);
}
module.exports = {
    resolveLatencyTriplet,
    fixed,
    padStart,
    padEnd
};
