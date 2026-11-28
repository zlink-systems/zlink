// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  resolveMultiMonitorHwm
} = require('../perf/multi/perf_multi_common');

test('multi monitor HWM resolves exact nonnegative byte values', () => {
  assert.equal(resolveMultiMonitorHwm({}), 4_096_000);
  assert.equal(resolveMultiMonitorHwm({ PERF_MONITOR_HWM_BYTES: '123' }), 123);
  assert.equal(resolveMultiMonitorHwm({
    PERF_MULTI_MONITOR_HWM_BYTES: '456',
    PERF_MONITOR_HWM_BYTES: '123'
  }), 456);
  assert.equal(resolveMultiMonitorHwm({ PERF_MULTI_MONITOR_HWM_BYTES: '0' }), 0);
  assert.equal(resolveMultiMonitorHwm({}, '789'), 789);
});

test('multi monitor HWM rejects invalid byte values', () => {
  for (const value of ['-1', '1.5', 'x', String(Number.MAX_SAFE_INTEGER + 1)]) {
    assert.throws(() => resolveMultiMonitorHwm({ PERF_MULTI_MONITOR_HWM_BYTES: value }),
      /--monitor-hwm-bytes/);
  }
});

test('multi monitor HWM rejects retired variable names', () => {
  for (const name of ['PERF_MULTI_MONITOR_HWM', 'PERF_MONITOR_HWM']) {
    for (const value of ['1024', '']) {
      assert.throws(() => resolveMultiMonitorHwm({ [name]: value }),
        new RegExp(`${name} is obsolete. Use ${name}_BYTES`));
    }
  }
});
