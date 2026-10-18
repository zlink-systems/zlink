'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');

for (const mode of ['completed', 'held', 'held-yield', 'seal']) {
  test(`shutdown returns while a Spot request handler is ${mode}`, async () => {
    const child = spawn(
      process.execPath,
      [path.join(__dirname, 'fixtures/spot-shutdown-execution-process.js'), mode],
      {
        stdio: ['ignore', 'pipe', 'pipe'],
        env: {
          ...process.env,
          ZLINK_TEST_FLOW_FILE: process.env.ZLINK_TEST_FLOW_PREFIX
            ? `${process.env.ZLINK_TEST_FLOW_PREFIX}-${mode}.jsonl`
            : ''
        }
      }
    );
    let stdout = '';
    let stderr = '';
    let observation;
    let expired = false;
    const completed = new Promise((resolve, reject) => {
      child.once('error', reject);
      child.stdout.setEncoding('utf8').on('data', (chunk) => {
        stdout += chunk;
        if (!observation && stdout.includes('shutdown-started')) {
          // This bounds the observation, not the runtime's 50 ms shutdown deadline.
          observation = setTimeout(() => {
            expired = true;
            child.kill('SIGKILL');
          }, 2000);
        }
      });
      child.stderr.setEncoding('utf8').on('data', (chunk) => {
        stderr += chunk;
      });
      child.once('close', (code, signal) => resolve({ code, signal }));
    });
    const startupBound = setTimeout(() => child.kill('SIGKILL'), 15000);
    try {
      const result = await completed;
      assert.equal(
        expired,
        false,
        `shutdown did not return after its deadline\n${stdout}\n${stderr}`
      );
      assert.equal(result.signal, null, stdout + stderr);
      assert.equal(result.code, 0, stdout + stderr);
      assert.match(stdout, /target-handler-entered/);
      assert.match(stdout, /shutdown-returned:/);
    } finally {
      clearTimeout(startupBound);
      clearTimeout(observation);
      if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
    }
  });
}
