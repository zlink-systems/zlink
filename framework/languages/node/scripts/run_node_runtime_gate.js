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
const failedTestFiles = [];
let expectedTestCount = 0;
let completedTestCount = 0;

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
  const result = runTestFile(relative, [
    '--test',
    '--test-timeout=600000',
    testFile
  ]);
  expectedTestCount += result.announced;
  completedTestCount += result.completed;
  if (!result.passed) failedTestFiles.push(relative);
}
releaseGateLock();
if (skipped.length > 0) {
  console.error(`Framework CI skipped ${skipped.length} test file(s): ${skipped.join(', ')}`);
  process.exit(2);
}
if (completedTestCount !== expectedTestCount) {
  console.error(
    `Runtime test aggregate is incomplete: announced ${expectedTestCount}, completed ${completedTestCount}.`
  );
  process.exit(1);
}
console.log(
  `-- runtime test integrity: announced=${expectedTestCount} completed=${completedTestCount}`
);
if (failedTestFiles.length > 0) {
  console.error(`Framework runtime tests failed in ${failedTestFiles.length} file(s): ${failedTestFiles.join(', ')}`);
  process.exit(1);
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

function runTestFile(label, args) {
  console.log(`-- ${label}`);
  const result = childProcess.spawnSync(process.execPath, args, {
    cwd: nodeRoot,
    encoding: 'utf8',
    env: process.env,
    maxBuffer: 64 * 1024 * 1024,
    timeout: 600000
  });
  process.stdout.write(result.stdout ?? '');
  process.stderr.write(result.stderr ?? '');

  const tap = inspectTap(result.stdout ?? '');
  const integrityErrors = [];
  if (tap.plan === undefined) integrityErrors.push('missing top-level TAP plan');
  if (tap.summary === undefined) integrityErrors.push('missing TAP test summary');
  if (tap.plan !== undefined && tap.plan !== tap.announced) {
    integrityErrors.push(`plan=${tap.plan}, announced=${tap.announced}`);
  }
  if (tap.plan !== undefined && tap.plan !== tap.completed) {
    integrityErrors.push(`plan=${tap.plan}, completed=${tap.completed}`);
  }
  if (tap.summary !== undefined && tap.summary !== tap.completed) {
    integrityErrors.push(`summary=${tap.summary}, completed=${tap.completed}`);
  }
  if (result.error) {
    integrityErrors.push(
      result.error.code === 'ETIMEDOUT'
        ? 'parent watchdog expired after 600000ms'
        : `runner error: ${result.error.message}`
    );
  }
  if (integrityErrors.length > 0) {
    console.error(`${label} produced incomplete TAP: ${integrityErrors.join('; ')}.`);
  }
  if (result.status !== 0 && !result.error) {
    console.error(`${label} failed with exit code ${result.status ?? 1}.`);
  }

  return {
    announced: tap.announced,
    completed: tap.completed,
    passed: result.status === 0 && integrityErrors.length === 0
  };
}

function inspectTap(output) {
  let announced = 0;
  let completed = 0;
  let plan;
  let summary;
  for (const line of output.split(/\r?\n/)) {
    if (/^# Subtest: /.test(line)) announced += 1;
    if (/^(?:ok|not ok) \d+ - /.test(line)) completed += 1;
    const planMatch = /^1\.\.(\d+)$/.exec(line);
    if (planMatch !== null) plan = Number(planMatch[1]);
    const summaryMatch = /^# tests (\d+)$/.exec(line);
    if (summaryMatch !== null) summary = Number(summaryMatch[1]);
  }
  return { announced, completed, plan, summary };
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
