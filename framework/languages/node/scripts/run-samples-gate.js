#!/usr/bin/env node
'use strict';

const childProcess = require('node:child_process');
const path = require('node:path');

const nodeRoot = path.resolve(__dirname, '..');
const args = process.argv.slice(2);
const dryRun = args[0] === '--dry-run';
const runnerArgs = dryRun ? args.slice(1) : args;
const platform = dryRun && process.env.ZLINK_TEST_PLATFORM
  ? process.env.ZLINK_TEST_PLATFORM
  : process.platform;
const windows = platform === 'win32';
const command = windows ? 'powershell.exe' : 'bash';
const commandArgs = windows
  ? [
      '-NoProfile',
      '-NonInteractive',
      '-ExecutionPolicy',
      'Bypass',
      '-File',
      path.join(nodeRoot, 'samples', 'run_samples.ps1'),
      ...runnerArgs
    ]
  : [path.join(nodeRoot, 'samples', 'run_samples.sh'), ...runnerArgs];

if (dryRun) {
  process.stdout.write(`${JSON.stringify({ command, args: commandArgs })}\n`);
  process.exit(0);
}

const result = childProcess.spawnSync(command, commandArgs, {
  cwd: nodeRoot,
  stdio: 'inherit',
  shell: false
});
if (result.error) throw result.error;
process.exit(result.status ?? 1);
