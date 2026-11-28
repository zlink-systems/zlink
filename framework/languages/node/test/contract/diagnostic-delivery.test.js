'use strict';

const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { mkdtempSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const test = require('node:test');

for (const mode of ['progress', 'overflow']) {
  test(`diagnostic worker: ${mode}`, (t) => {
    const run = mkdtempSync(join(tmpdir(), 'zlink-diagnostic-delivery-'));
    t.after(() => rmSync(run, { recursive: true, force: true }));
    const result = spawnSync(
      process.execPath,
      ['--require', join(__dirname, 'fixtures/telemetry-worker-preload.cjs'),
        join(__dirname, 'fixtures/diagnostic-worker-case.cjs')],
      {
        env: {
          ...process.env,
          ZLINK_TEST_TELEMETRY_CASE: `diagnostic-${mode}`,
          ZLINK_TEST_TELEMETRY_RECORDS: join(run, 'records.jsonl'),
          ZLINK_TEST_TELEMETRY_GATE: join(run, 'release')
        },
        encoding: 'utf8',
        timeout: 25000
      }
    );
    t.diagnostic(result.stdout + result.stderr);
    assert.ifError(result.error);
    assert.equal(result.status, 0);
    if (mode === 'progress') {
      assert.match(result.stderr, /\[zlink\.runtime\.relocation\.drain_failed\]/);
      assert.match(result.stderr, /\[zlink\.runtime\.relocation\.source_cleanup_failed\]/);
    }
  });
}
