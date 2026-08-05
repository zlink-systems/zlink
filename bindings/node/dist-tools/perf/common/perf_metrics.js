// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
exports.integerEnv = exports.stampPayload = exports.summarizeMetrics = exports.sleepMillis = exports.decodeMetricHeader = exports.currentEpochNs = exports.createRunId = exports.createPayload = exports.createMetricCollector = exports.HEADER_SIZE = void 0;
const args = require('./perf_args');
const measurement = require('./perf_measurement');
const report = require('./perf_report');
const runtimePolicy = require('./perf_runtime_policy');
exports.HEADER_SIZE = measurement.HEADER_SIZE, exports.createMetricCollector = measurement.createMetricCollector, exports.createPayload = measurement.createPayload, exports.createRunId = measurement.createRunId, exports.currentEpochNs = measurement.currentEpochNs, exports.decodeMetricHeader = measurement.decodeMetricHeader, exports.sleepMillis = measurement.sleepMillis, exports.summarizeMetrics = measurement.summarizeMetrics, exports.stampPayload = measurement.stampPayload;
exports.integerEnv = runtimePolicy.integerEnv;
module.exports = {
    ...args,
    ...measurement,
    ...report,
    ...runtimePolicy
};
