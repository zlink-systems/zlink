// SPDX-License-Identifier: MPL-2.0

'use strict';

const args = require('./perf_args');
const measurement = require('./perf_measurement');
const report = require('./perf_report');
const runtimePolicy = require('./perf_runtime_policy');

export const {
  HEADER_SIZE,
  createMetricCollector,
  createPayload,
  createRunId,
  currentEpochNs,
  decodeMetricHeader,
  sleepMillis,
  summarizeMetrics,
  stampPayload
} = measurement;

export const {
  integerEnv
} = runtimePolicy;

module.exports = {
  ...args,
  ...measurement,
  ...report,
  ...runtimePolicy
};
