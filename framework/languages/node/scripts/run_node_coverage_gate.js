#!/usr/bin/env node
const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { acquireNodeTestGateLock } = require('./node-test-gate-lock');

const nodeRoot = path.resolve(__dirname, '..');
const coverageLineTarget = process.env.ZLINK_NODE_COVERAGE_LINES ?? '80';
const expectedMajor = Number(process.env.ZLINK_EXPECT_NODE_MAJOR ?? '0');
const actualMajor = Number(process.versions.node.split('.')[0]);
const milestoneContractsOnly = process.argv.includes('--milestone-contracts-only');
const unknownArguments = process.argv.slice(2)
  .filter((argument) => argument !== '--milestone-contracts-only');
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
if (unknownArguments.length > 0) {
  console.error(`Unknown coverage gate argument: ${unknownArguments.join(', ')}`);
  process.exit(1);
}

const releaseGateLock = acquireNodeTestGateLock(nodeRoot, 'coverage');
run(process.execPath, [
  path.resolve(nodeRoot, 'node_modules/typescript/bin/tsc'),
  '-b',
  'tsconfig.build.json'
]);
for (const milestone of ['m5-foundation', 'm6a-runtime', 'm6b-runtime', 'm6c-runtime']) {
  run(process.execPath, [
    path.resolve(nodeRoot, 'node_modules/typescript/bin/tsc'),
    '-p',
    `tsconfig.${milestone}.json`
  ]);
}
const milestoneContractTests = prepareMilestoneContractTests();
const testFiles = [
  ...(milestoneContractsOnly ? [] : listTestFiles(path.join(nodeRoot, 'test'))),
  ...milestoneContractTests
].filter((testFile) => !isSkippedTestFile(nodeRoot, testFile));
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

function prepareMilestoneContractTests() {
  const outputRoot = path.join(nodeRoot, 'build', 'coverage-contracts');
  fs.rmSync(outputRoot, { recursive: true, force: true });

  const contractFiles = [];
  for (const [milestone, buildName] of [
    ['m5', 'm5-foundation'],
    ['m6a', 'm6a-runtime'],
    ['m6b', 'm6b-runtime'],
    ['m6c', 'm6c-runtime']
  ]) {
    const sourceRoot = path.join(nodeRoot, 'test', milestone);
    const compiledRoot = path.join(
      nodeRoot,
      'build',
      buildName,
      'languages',
      'node',
      'test',
      milestone
    );
    const outputDirectory = path.join(outputRoot, 'test', milestone);
    fs.mkdirSync(outputDirectory, { recursive: true });

    for (const entry of fs.readdirSync(sourceRoot, { withFileTypes: true })) {
      if (!entry.isFile() || !entry.name.endsWith('.contract.ts')) {
        continue;
      }

      const sourceFile = path.join(sourceRoot, entry.name);
      const compiledFile = path.join(compiledRoot, entry.name.replace(/\.ts$/, '.js'));
      const outputFile = path.join(outputDirectory, entry.name.replace(/\.ts$/, '.js'));
      if (!fs.existsSync(compiledFile)) {
        throw new Error(`Missing compiled milestone contract for ${sourceFile}.`);
      }
      const compiled = rewriteMilestoneContractImports(fs.readFileSync(compiledFile, 'utf8'));
      fs.writeFileSync(outputFile, compiled);
      contractFiles.push(outputFile);
    }
  }
  return contractFiles.sort();
}

function rewriteMilestoneContractImports(source) {
  return source
    .replaceAll(
      '../../packages/framework/src/',
      '../../../../packages/framework/dist/'
    )
    .replaceAll(
      '../../../../runtime/protocol/generated/node/service_wire_constants',
      '../../../../../../runtime/protocol/generated/node/service_wire_constants'
    );
}
