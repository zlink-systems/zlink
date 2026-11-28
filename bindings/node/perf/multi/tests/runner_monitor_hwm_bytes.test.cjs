// SPDX-License-Identifier: MPL-2.0

const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const test = require('node:test');
const runner = require.resolve('../../../dist-tools/perf/multi/run_benchmarks.js');

test('multi runner resolves monitor HWM bytes and rejects retired names', () => {
  const run = (options, overrides = {}) => {
    const env = { ...process.env };
    for (const name of ['PERF_MULTI_MONITOR_HWM', 'PERF_MONITOR_HWM',
      'PERF_MULTI_MONITOR_HWM_BYTES', 'PERF_MONITOR_HWM_BYTES']) {
      delete env[name];
    }
    // The invalid pattern stops the runner after parsing, before any measurement.
    const result = spawnSync(process.execPath, ['-e', `
      process.on('exit', () => console.log(process.env.PERF_MULTI_MONITOR_HWM_BYTES));
      require(${JSON.stringify(runner)});
    `, '--', 'runner', '--pattern', 'INVALID', ...options], {
      encoding: 'utf8', env: { ...env, ...overrides }
    });
    assert.equal(result.status, 1, result.error?.message || result.stderr);
    return result;
  };

  for (const [options, env, expected] of [
    [['--monitor-hwm-bytes', '0'], { PERF_MULTI_MONITOR_HWM_BYTES: '9' }, '0'],
    [[], { PERF_MULTI_MONITOR_HWM_BYTES: '8', PERF_MONITOR_HWM_BYTES: '7' }, '8'],
    [[], { PERF_MULTI_MONITOR_HWM_BYTES: '', PERF_MONITOR_HWM_BYTES: '7' }, '7'],
    [[], {}, '4096000']
  ]) {
    const result = run(options, env);
    assert.match(result.stderr, /unsupported multi pattern: INVALID/);
    assert.equal(result.stdout.trim(), expected);
  }
  assert.match(run(['--monitor-hwm', '1']).stderr, /unsupported argument: --monitor-hwm/);
  assert.match(run(['--monitor-hwm-bytes', '-1']).stderr, /nonnegative/);
  assert.match(run(['--monitor-hwm-bytes']).stderr, /requires a value/);
  assert.match(run([], { PERF_MULTI_MONITOR_HWM_BYTES: '-1' }).stderr, /nonnegative/);
  for (const name of ['PERF_MULTI_MONITOR_HWM', 'PERF_MONITOR_HWM']) {
    assert.match(run(['--monitor-hwm-bytes', '1'], { [name]: '' }).stderr,
      new RegExp(`${name} is obsolete`));
  }
});
