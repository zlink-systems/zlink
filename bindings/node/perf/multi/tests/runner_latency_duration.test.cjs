// SPDX-License-Identifier: MPL-2.0

const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const test = require('node:test');
const runner = require.resolve('../../../dist-tools/perf/multi/run_benchmarks.js');

test('multi runner exports the resolved positive latency duration', () => {
  const run = (options, environment = '') => {
    // The invalid pattern stops the runner after parsing, before any measurement.
    const result = spawnSync(process.execPath, ['-e', `
      process.on('exit', () => console.log(process.env.PERF_MULTI_LATENCY_DURATION_SECONDS));
      require(${JSON.stringify(runner)});
    `, '--', 'runner', '--pattern', 'INVALID', '--duration', '7', ...options], {
      encoding: 'utf8',
      env: { ...process.env, PERF_MULTI_LATENCY_DURATION_SECONDS: environment }
    });
    assert.equal(result.status, 1, result.error?.message || result.stderr);
    return result;
  };

  for (const [options, env, expected] of [
    [[], '', '7'],
    [[], '4', '4'],
    [['--latency-duration', '3'], '0', '3'],
    [['--latency-duration=009'], '', '009']
  ]) {
    const result = run(options, env);
    assert.match(result.stderr, /unsupported multi pattern: INVALID/);
    assert.equal(result.stdout.trim(), expected);
  }
  for (const invalid of ['0', '-1', '1.5', '1e2', '']) {
    assert.match(run([`--latency-duration=${invalid}`]).stderr, /must be a positive integer/);
  }
  assert.match(run(['--latency-duration']).stderr, /requires a value/);
  assert.match(run([], '0').stderr, /must be a positive integer/);
});
