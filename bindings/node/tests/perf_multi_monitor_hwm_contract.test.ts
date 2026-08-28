// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  resolveMultiMonitorHwm
} = require('../perf/multi/perf_multi_common');

test('multi monitor HWM resolves exact nonnegative byte values', () => {
  assert.equal(resolveMultiMonitorHwm({}), 4_096_000);
  assert.equal(resolveMultiMonitorHwm({ PERF_MONITOR_HWM: '123' }), 123);
  assert.equal(resolveMultiMonitorHwm({
    PERF_MULTI_MONITOR_HWM: '456',
    PERF_MONITOR_HWM: '123'
  }), 456);
  assert.equal(resolveMultiMonitorHwm({ PERF_MULTI_MONITOR_HWM: '0' }), 0);
  assert.equal(resolveMultiMonitorHwm({
    PERF_MULTI_MONITOR_HWM: '-1',
    PERF_MONITOR_HWM: '789'
  }), 789);
  assert.equal(resolveMultiMonitorHwm({
    PERF_MULTI_MONITOR_HWM: String(Number.MAX_SAFE_INTEGER + 1)
  }), 4_096_000);
});
