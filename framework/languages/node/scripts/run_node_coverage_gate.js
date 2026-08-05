#!/usr/bin/env node
const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { acquireNodeTestGateLock } = require('./node-test-gate-lock');

const nodeRoot = path.resolve(__dirname, '..');
const coverageLineTarget = process.env.ZLINK_NODE_COVERAGE_LINES ?? '80';
const expectedMajor = Number(process.env.ZLINK_EXPECT_NODE_MAJOR ?? '0');
const actualMajor = Number(process.versions.node.split('.')[0]);
const skippedTestFiles = new Set(
  (process.env.ZLINK_NODE_COVERAGE_GATE_SKIP_TESTS ?? '')
    .split(',')
    .map((value) => value.trim())
    .filter((value) => value.length > 0)
);

if (expectedMajor !== 0 && actualMajor !== expectedMajor) {
  console.error(`Expected Node ${expectedMajor}, got ${process.version}.`);
  process.exit(1);
}

const releaseGateLock = acquireNodeTestGateLock(nodeRoot, 'coverage');
run(process.execPath, [
  path.resolve(nodeRoot, 'node_modules/typescript/bin/tsc'),
  '-b',
  'tsconfig.build.json'
]);
const testFiles = listTestFiles(path.join(nodeRoot, 'test'))
  .filter((testFile) => !isSkippedTestFile(nodeRoot, testFile));
run(process.execPath, [
  '--test',
  '--test-force-exit',
  '--test-concurrency=1',
  '--experimental-test-coverage',
  "--test-coverage-include=packages/*/dist/**/*.js",
  `--test-coverage-lines=${coverageLineTarget}`,
  ...testFiles
]);
releaseGateLock();

function run(command, args) {
  const result = childProcess.spawnSync(command, args, {
    cwd: nodeRoot,
    stdio: 'inherit',
    env: process.env
  });
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

function isSkippedTestFile(root, testFile) {
  const relative = path.relative(root, testFile).split(path.sep).join('/');
  return skippedTestFiles.has(relative) || skippedTestFiles.has(path.basename(testFile));
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
