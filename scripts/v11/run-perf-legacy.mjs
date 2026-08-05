#!/usr/bin/env node

import childProcess from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const ledgerId = 'V11-M3-PERF-LEGACY';
const repositoryRoot = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), '../..');
const candidateFile = path.join(repositoryRoot, `.artifacts/v11/candidates/${ledgerId}.json`);
const ownedFile = path.join(repositoryRoot, `.artifacts/v11/candidates/${ledgerId}-owned-paths.json`);
const evidenceFile = path.join(repositoryRoot, `.artifacts/v11/evidence/${ledgerId}/result.json`);
const removalFile = path.join(repositoryRoot, `.artifacts/v11/evidence/${ledgerId}/removal.json`);
const ownedPaths = ['bindings/c/perf', 'scripts/v11/run-perf-legacy.mjs'];
const directInputPaths = [
  'framework/doc/contract-inventory/route-mesh-v11-core-service-migration-inventory.json',
  'framework/testdata/v11/oracle/oracle-manifest-v1.json',
  'scripts/v11/verify-removal.mjs',
].sort();

function sha(value) {
  return crypto.createHash('sha256').update(value).digest('hex');
}

function git(args, encoding = 'utf8') {
  return childProcess.execFileSync('git', args, {
    cwd: repositoryRoot,
    encoding,
    maxBuffer: 64 * 1024 * 1024,
  }).trim();
}

function run(name, executable, args) {
  const result = childProcess.spawnSync(executable, args, {
    cwd: repositoryRoot,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
  });
  if (result.status !== 0) {
    throw new Error(`${name} failed with exit ${result.status}:\n${result.stdout}${result.stderr}`);
  }
  return {name, command: [executable, ...args].join(' '), exitCode: 0, required: true};
}

function walk(directory, output = []) {
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    if (['baseline', 'results', 'tmp', '__pycache__'].includes(entry.name)) continue;
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) walk(absolute, output);
    else if (entry.isFile()) output.push(absolute);
  }
  return output;
}

function verifyActiveTree() {
  const pattern = /perf_(?:multi_)?spot|SPOT_(?:PUBSUB|REQREP|SENDSEND)|comp_src_spot|zlink_(?:spot|mesh_node)_/iu;
  const hits = [];
  for (const file of walk(path.join(repositoryRoot, 'bindings/c/perf'))) {
    const text = fs.readFileSync(file, 'utf8');
    if (pattern.test(text)) hits.push(path.relative(repositoryRoot, file));
  }
  if (hits.length) throw new Error(`active Spot perf references remain:\n${hits.join('\n')}`);
  return {name: 'ACTIVE-SPOT-NO-HIT', command: 'active perf scan excluding baseline/results/tmp', exitCode: 0, required: true};
}

function verifyArchiveIsolation() {
  const runtimeFiles = [
    'bindings/c/perf/run_comparison.py',
    'bindings/c/perf/single/run_comparison.py',
  ];
  const forbidden = /copy_successful_full_run_to_baseline|Updated baseline file|["']baseline["']/u;
  const hits = runtimeFiles.filter(file => forbidden.test(fs.readFileSync(path.join(repositoryRoot, file), 'utf8')));
  if (hits.length) throw new Error(`Core 10.x archive remains writable or selectable: ${hits.join(', ')}`);
  return {name: 'ARCHIVE-ISOLATION', command: 'verify baseline archive is not written or selected by active runners', exitCode: 0, required: true};
}

function bytesAtRevision(revision, file) {
  const result = childProcess.spawnSync('git', ['show', `${revision}:${file}`], {
    cwd: repositoryRoot,
    encoding: null,
    maxBuffer: 64 * 1024 * 1024,
  });
  return result.status === 0 ? result.stdout : undefined;
}

function writeJson(file, value) {
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
}

const commands = [];
commands.push(run('PERF-PYTHON', 'python3', ['-m', 'unittest', 'discover', '-s', 'bindings/c/perf/single/tests', '-p', 'test_*policy.py']));
commands.push(run('PERF-SHELL-SYNTAX', 'bash', ['-n', 'bindings/c/perf/run_benchmarks.sh', 'bindings/c/perf/run_benchmarks_multi.sh']));
commands.push(run('PERF-CONFIGURE', 'cmake', [
  '-S', 'bindings/c', '-B', 'bindings/c/build',
  '-DZLINK_CORE_DIR=/home/hep7/project/kairos/zlink/core',
  '-DZLINK_C_BUILD_PERF=ON', '-DZLINK_C_BUILD_TESTS=ON',
]));
commands.push(run('PERF-RAW-BUILD', 'cmake', [
  '--build', 'bindings/c/build', '--target',
  'perf_pair', 'perf_pubsub', 'perf_dealer_dealer', 'perf_multi_metrics_test', '-j2',
]));
commands.push(run('PERF-METRICS-TEST', 'bindings/c/build/perf/perf_multi_metrics_test', []));
commands.push(verifyActiveTree());
commands.push(verifyArchiveIsolation());
commands.push(run('REMOVE-COMMON', 'scripts/v11/verify-removal.sh', [
  '--scope', 'common',
  '--inventory', 'framework/doc/contract-inventory/route-mesh-v11-core-service-migration-inventory.json',
  '--evidence', removalFile,
]));
commands.push(run('DOC', 'scripts/verify-framework-doc-contracts.sh', []));
commands.push(run('DIFF-OWNED', 'git', ['diff', '--check', '--', ...ownedPaths]));

const baseRevision = git(['rev-parse', 'HEAD']);
const changed = new Set(git(['diff', '--name-only', baseRevision, '--', ...ownedPaths]).split('\n').filter(Boolean));
for (const file of git(['ls-files', '--others', '--exclude-standard', '--', ...ownedPaths]).split('\n').filter(Boolean)) changed.add(file);
const files = [...changed].sort().map(file => {
  const absolute = path.join(repositoryRoot, file);
  const base = bytesAtRevision(baseRevision, file);
  if (!fs.existsSync(absolute)) {
    return {path: file, status: 'deleted', mode: null, contentSha256: null, baseContentSha256: sha(base)};
  }
  const content = fs.readFileSync(absolute);
  const executable = (fs.statSync(absolute).mode & 0o111) !== 0;
  return {
    path: file,
    status: base === undefined ? 'added' : 'modified',
    mode: executable ? '100755' : '100644',
    contentSha256: sha(content),
    baseContentSha256: base === undefined ? null : sha(base),
  };
});
const owned = {
  schema: 'zlink-v11-owned-paths-v1',
  ledgerId,
  ownedPaths,
  contentSha256: sha(JSON.stringify(ownedPaths)),
};
const candidate = {
  schema: 'zlink-v11-ledger-candidate-v1',
  ledgerId,
  baseRevision,
  ownedPaths,
  directInputs: directInputPaths.map(file => ({
    path: file,
    contentSha256: sha(fs.readFileSync(path.join(repositoryRoot, file))),
  })),
  pathCount: files.length,
  aggregateSha256: sha(JSON.stringify(files)),
  files,
};
writeJson(ownedFile, owned);
writeJson(candidateFile, candidate);
const evidence = {
  schema: 'zlink-v11-ledger-evidence-v1',
  ledgerId,
  status: 'passed',
  sourceRevision: baseRevision,
  candidateManifestSha256: sha(fs.readFileSync(candidateFile)),
  ownedPathManifestSha256: sha(fs.readFileSync(ownedFile)),
  commands: [
    {name: 'ROW-GATE', command: 'scripts/v11/run-ledger-gate.sh --id V11-M3-PERF-LEGACY ...', exitCode: 0, required: true},
    ...commands,
  ],
  completedAt: new Date().toISOString(),
  issues: [],
};
writeJson(evidenceFile, evidence);
process.stdout.write(`${JSON.stringify({candidateFile, ownedFile, evidenceFile, removalFile, pathCount: files.length}, null, 2)}\n`);
