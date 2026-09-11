#!/usr/bin/env node
'use strict';

const childProcess = require('node:child_process');
const path = require('node:path');

const nodeRoot = path.resolve(__dirname, '..');
const dryRun = process.argv[2] === '--dry-run';
const platform = dryRun && process.env.ZLINK_TEST_PLATFORM
  ? process.env.ZLINK_TEST_PLATFORM
  : process.platform;
const npm = platform === 'win32' ? 'npm.cmd' : 'npm';
const commands = [
  {
    label: 'build Node Framework',
    command: npm,
    args: ['run', 'build']
  },
  {
    label: 'build .NET cross-language host',
    command: 'dotnet',
    args: [
      'build',
      path.resolve(nodeRoot, '../dotnet/cross-language/Zlink.Framework.TestHost/Zlink.Framework.TestHost.csproj'),
      '--framework',
      'net8.0'
    ]
  },
  {
    label: 'run cross-language smoke',
    command: process.execPath,
    args: [path.join(nodeRoot, 'cross-language', 'node_dotnet_smoke.js')],
    env: { ...process.env, ZLINK_DOTNET_TESTHOST_NO_BUILD: '1' }
  }
];

if (dryRun) {
  for (const command of commands) {
    process.stdout.write(`${JSON.stringify({ command: command.command, args: command.args })}\n`);
  }
  process.exit(0);
}

for (const item of commands) {
  process.stdout.write(`-- ${item.label}\n`);
  const result = childProcess.spawnSync(item.command, item.args, {
    cwd: nodeRoot,
    stdio: 'inherit',
    env: item.env ?? process.env,
    shell: platform === 'win32' && item.command.endsWith('.cmd')
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}
