#!/usr/bin/env node
'use strict';

const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const nodeRoot = path.resolve(__dirname, '..');
//  One invocation runs one sample. A per-language batch runner let a stalled sample hold
//  the whole run and made interference between samples look like a defect in any one of
//  them (#405, e106104ffe), so `samples/run_samples.sh` and `samples/run_samples.ps1` are
//  gone. This gate keeps the release entry point (`npm run verify:samples`) by walking the
//  per-sample runners the framework gate calls, one child process per sample.
//
//  Signals are deliberately not relayed here. The runner this gate spawns is the same
//  `samples/<Sample>/run_sample.sh` the framework gate invokes directly, and it owns its
//  own process tree: it execs `run-sample.mjs`, which reaps its children and removes its
//  run directory on SIGINT/SIGTERM. That contract is proven against the runner itself in
//  `samples/ZoneWorld/Runner/test/runner-cleanup.test.js`, not against a wrapper.
const maintainedSamples = [
  'TicTacToe.Ts',
  'Bingo.Ts',
  'DeliveryDispatch.Ts',
  'SupportChat.Ts',
  'GameQuest.Ts',
  'ShoppingMall.Ts',
  'ZoneWorld'
];

const args = process.argv.slice(2);
const dryRun = args[0] === '--dry-run';
const selected = dryRun ? args.slice(1) : args;
const samples = selected.length > 0 ? selected : maintainedSamples;
const platform = dryRun && process.env.ZLINK_TEST_PLATFORM
  ? process.env.ZLINK_TEST_PLATFORM
  : process.platform;
const windows = platform === 'win32';

const invocations = samples.map((sample) => {
  const runner = path.join(nodeRoot, 'samples', sample, windows ? 'run_sample.ps1' : 'run_sample.sh');
  if (!fs.existsSync(runner)) {
    process.stderr.write(`Unknown Node sample '${sample}'.\n`);
    process.exit(1);
  }
  return windows
    ? {
        sample,
        command: 'powershell.exe',
        args: ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', runner]
      }
    : { sample, command: 'bash', args: [runner] };
});

if (dryRun) {
  for (const invocation of invocations) {
    process.stdout.write(`${JSON.stringify({ command: invocation.command, args: invocation.args })}\n`);
  }
  process.exit(0);
}

for (const invocation of invocations) {
  process.stdout.write(`sample ${invocation.sample} start\n`);
  const result = childProcess.spawnSync(invocation.command, invocation.args, {
    cwd: nodeRoot,
    stdio: 'inherit',
    shell: false
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
  process.stdout.write(`sample ${invocation.sample} completed\n`);
}
