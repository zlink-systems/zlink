#!/usr/bin/env node
const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { acquireNodeTestGateLock } = require('./node-test-gate-lock');

const nodeRoot = path.resolve(__dirname, '..');
const eslintEntry = path.resolve(nodeRoot, 'node_modules/eslint/bin/eslint.js');
const expectedMajor = Number(process.env.ZLINK_EXPECT_NODE_MAJOR ?? '0');
const actualMajor = Number(process.versions.node.split('.')[0]);
const skippedTestFiles = new Set(
  (process.env.ZLINK_NODE_RUNTIME_GATE_SKIP_TESTS ?? '')
    .split(',')
    .map((value) => value.trim())
    .filter((value) => value.length > 0)
);
const skipped = [];

if (expectedMajor !== 0 && actualMajor !== expectedMajor) {
  console.error(`Expected Node ${expectedMajor}, got ${process.version}.`);
  process.exit(1);
}

const releaseGateLock = acquireNodeTestGateLock(nodeRoot, 'runtime');

run('build', process.platform === 'win32' ? 'npm.cmd' : 'npm', ['run', 'build']);
run('typecheck', process.execPath, [
  path.resolve(nodeRoot, 'node_modules/typescript/bin/tsc'),
  '-p',
  'tsconfig.json',
  '--noEmit'
]);
run('lint', process.execPath, [
  eslintEntry,
  'packages/*/src/**/*.ts',
  'samples/**/*.ts'
]);
for (const testFile of listTestFiles(path.join(nodeRoot, 'test'))) {
  const relative = relativePath(nodeRoot, testFile);
  if (skippedTestFiles.has(relative) || skippedTestFiles.has(path.basename(testFile))) {
    console.log(`-- ${relative} # SKIP explicitly requested; skipped tests are not a pass`);
    skipped.push(relative);
    continue;
  }
  //  타임아웃이 없으면 suite가 멈췄을 때 게이트가 실패가 아니라 "영원히 진행 중"이
  //  된다. 실제로 그 상태에서 부분 로그를 통과로 오인하기 쉬웠다.
  run(relative, process.execPath, [
    '--test',
    '--test-force-exit',
    '--test-timeout=600000',
    testFile
  ]);
}
releaseGateLock();
if (skipped.length > 0) {
  console.error(`Framework CI skipped ${skipped.length} test file(s): ${skipped.join(', ')}`);
  process.exit(2);
}

function relativePath(base, file) {
  return path.relative(base, file).split(path.sep).join('/');
}

function run(label, command, args) {
  console.log(`-- ${label}`);
  const result = childProcess.spawnSync(command, args, {
    cwd: nodeRoot,
    stdio: 'inherit',
    env: process.env
  });
  if (result.error) {
    console.error(`Failed to run ${label}: ${result.error.message}`);
  }
  if (result.status !== 0) {
    console.error(`${label} failed with exit code ${result.status ?? 1}.`);
    process.exit(result.status ?? 1);
  }
}

function listTestFiles(root) {
  const files = [];
  visit(root);
  return files.sort();

  function visit(current) {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const fullPath = path.join(current, entry.name);
      if (entry.isDirectory()) {
        visit(fullPath);
      } else if (entry.isFile() && entry.name.endsWith('.test.js')) {
        files.push(fullPath);
      }
    }
  }
}
