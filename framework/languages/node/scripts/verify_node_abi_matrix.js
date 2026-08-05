#!/usr/bin/env node
const fs = require('node:fs');
const path = require('node:path');

const nodeRoot = path.resolve(__dirname, '..');
const repoRoot = path.resolve(nodeRoot, '..', '..', '..');
const nodeDocRoot = path.join(repoRoot, 'framework', 'doc', 'framework', 'node');

const requiredPlatforms = [
  'win-x64',
  'win-arm64',
  'linux-x64',
  'linux-arm64',
  'darwin-x64',
  'darwin-arm64'
];
const requiredNodeVersions = ['20', '22'];

const workflowPath = path.join(repoRoot, '.github', 'workflows', 'framework-node.yml');
const packagePath = path.join(nodeRoot, 'package.json');
const regressionMatrixPath = path.join(nodeDocRoot, 'internals', 'regression-test-matrix.ko.md');

const errors = [];

const workflow = read(workflowPath);
const packageJson = JSON.parse(read(packagePath));
const regressionMatrix = read(regressionMatrixPath);

for (const platform of requiredPlatforms) {
  requireText(workflow, platform, `${path.relative(repoRoot, workflowPath)} must include ${platform}`);
  requireText(regressionMatrix, platform, `${path.relative(repoRoot, regressionMatrixPath)} must include ${platform}`);
}

for (const version of requiredNodeVersions) {
  requireWorkflowNodeVersion(workflow, version);
}

requireWorkflowCommand(
  'npm --prefix framework/languages/node run verify:ci',
  'framework/languages/node',
  'npm run verify:ci',
  'framework-node workflow must run verify:ci');
requireWorkflowCommand(
  'npm --prefix framework/languages/node run verify:cross-language',
  'framework/languages/node',
  'npm run verify:cross-language',
  'framework-node workflow must run verify:cross-language');
requireScript('verify:ci');
requireScript('verify:p0');
requireScript('verify:samples');
requireScript('verify:runtime-matrix');
requireScript('verify:cross-language');
requireScript('verify:abi-matrix');
requireScript('verify:release');
requireScriptText('verify:ci', 'run_node_framework_ci_gate.js');
requireScriptText('verify:release', 'npm run verify:abi-matrix');
requireScriptText('verify:release', 'npm run verify:p0');
requireScriptText('verify:release', 'npm run verify:samples');
requireScriptText('verify:release', 'npm run verify:runtime-matrix');
requireScriptText('verify:release', 'npm run verify:cross-language');

if (errors.length > 0) {
  for (const error of errors) {
    console.error(error);
  }
  process.exit(1);
}

console.log('Node framework ABI matrix gate is declared and linked.');

function read(file) {
  return fs.readFileSync(file, 'utf8');
}

function requireText(text, needle, message) {
  if (!text.includes(needle)) {
    errors.push(message);
  }
}

function requireWorkflowCommand(legacyCommand, workingDirectory, runCommand, message) {
  if (workflow.includes(legacyCommand)) {
    return;
  }
  if (workflow.includes(`working-directory: ${workingDirectory}`) &&
      workflow.includes(`run: ${runCommand}`)) {
    return;
  }
  errors.push(message);
}

function requireWorkflowNodeVersion(workflowText, version) {
  const pattern = new RegExp(`(?:^|\\n)\\s*-\\s*${version}\\s*(?:\\n|$)`);
  if (!pattern.test(workflowText)) {
    errors.push(`.github/workflows/framework-node.yml must include Node ${version}`);
  }
}

function requireScript(name) {
  if (typeof packageJson.scripts?.[name] !== 'string') {
    errors.push(`framework/languages/node/package.json must define ${name}`);
  }
}

function requireScriptText(name, text) {
  const script = packageJson.scripts?.[name];
  if (typeof script !== 'string' || !script.includes(text)) {
    errors.push(`framework/languages/node/package.json ${name} must include ${text}`);
  }
}
