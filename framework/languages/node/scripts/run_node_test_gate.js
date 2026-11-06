#!/usr/bin/env node
const childProcess = require('node:child_process');
const { nodeTestConcurrencyArgs } = require('./node-test-options');

const result = childProcess.spawnSync(
  process.execPath,
  ['--test', ...nodeTestConcurrencyArgs(), ...process.argv.slice(2)],
  { stdio: 'inherit', env: process.env }
);

if (result.error) {
  console.error(`Failed to start Node test runner: ${result.error.message}`);
  process.exitCode = 1;
} else {
  process.exitCode = result.status ?? 1;
}
