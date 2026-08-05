#!/usr/bin/env node

import crypto from 'node:crypto';
import childProcess from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import {fileURLToPath} from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, '..', '..');
const defaultInventory = 'framework/doc/contract-inventory/route-mesh-v11-core-service-migration-inventory.json';
const defaultManifest = 'framework/testdata/v11/removal/core-removal-manifest-v1.json';
const defaultCandidate = '.artifacts/v11/candidates/V11-M2-CORE-READINESS.json';
const defaultOwnedPaths = '.artifacts/v11/candidates/V11-M2-CORE-READINESS-owned-paths.json';
const ownedRepositoryPaths = [
  'framework/testdata/v11/removal/core-removal-manifest-v1.json',
  'scripts/v11/run-core-readiness.mjs',
];
const allowedSections = ['corePublicSymbols', 'coreExportSymbols', 'files'];
const allowedDispositions = new Set([
  'target-contract',
  'raw-core-prerequisite',
  'superseded',
  'remove',
  'retain-10x-only',
]);

function parseArguments(argv) {
  const options = {
    inventory: defaultInventory,
    manifest: defaultManifest,
    mode: 'check',
    evidence: undefined,
    candidate: defaultCandidate,
    ownedPaths: defaultOwnedPaths,
    selfTest: false,
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--inventory') options.inventory = argv[++index];
    else if (argument === '--manifest') options.manifest = argv[++index];
    else if (argument === '--evidence') options.evidence = argv[++index];
    else if (argument === '--candidate-manifest') options.candidate = argv[++index];
    else if (argument === '--owned-path-manifest') options.ownedPaths = argv[++index];
    else if (argument === '--write') options.mode = 'write';
    else if (argument === '--check') options.mode = 'check';
    else if (argument === '--self-test') options.selfTest = true;
    else throw new Error(`unsupported argument: ${argument}`);
  }
  for (const [name, value] of Object.entries(options)) {
    if (name !== 'evidence' && name !== 'selfTest' && value === undefined) {
      throw new Error(`missing value for --${name}`);
    }
  }
  return options;
}

function resolveRepositoryPath(value) {
  return path.isAbsolute(value) ? value : path.join(repositoryRoot, value);
}

function repositoryPath(value) {
  const relative = path.relative(repositoryRoot, value).split(path.sep).join('/');
  return relative.startsWith('../') ? value : relative;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function stableJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function sha256(value) {
  return crypto.createHash('sha256').update(value).digest('hex');
}

function classifyRecord(section, record) {
  const text = `${record.file ?? ''} ${record.symbol ?? ''} ${record.action ?? ''}`;
  if (section === 'corePublicSymbols' || section === 'coreExportSymbols') {
    return /heartbeat/i.test(text) ? 'zmp-heartbeat-remove' : 'core-service-remove';
  }
  if (record.scope === 'shared-perf') {
    return record.disposition === 'retain-10x-only'
      ? 'core-10x-perf-archive-retain'
      : 'active-service-perf-remove';
  }
  if (record.action === 'retain-or-rename-generic-scheduler-and-remove-service-specific-state'
      || record.action === 'retain-generic-scheduler-regression-and-remove-service-specific-assertions') {
    return 'generic-timer-retain-service-fragment-remove';
  }
  if (record.disposition === 'raw-core-prerequisite' && /(?:timer|scheduler)/i.test(record.file ?? '')) {
    return 'generic-timer-retain-service-fragment-remove';
  }
  if (record.disposition === 'raw-core-prerequisite' && /monitor/i.test(record.file ?? '')) {
    return 'raw-monitor-retain-service-fragment-remove';
  }
  if (record.action === 'remove-zmp-heartbeat-option-frame-timer-or-test') {
    return 'zmp-heartbeat-remove';
  }
  if (record.disposition === 'raw-core-prerequisite') {
    return 'raw-core-retain-service-fragment-remove';
  }
  return 'core-service-remove-or-rewrite';
}

function heartbeatComponent(record) {
  if (record.classification !== 'zmp-heartbeat-remove') return undefined;
  if ((record.file ?? '').startsWith('core/tests/')) return 'test';
  if ((record.file ?? '').includes('/protocol/')) return 'frame-codec';
  if (/(?:\/engine\/|\/transports\/ws\/)/u.test(record.file ?? '')) return 'engine-timer';
  return 'option';
}

function compactRecord(section, record) {
  const compact = {
    id: record.id,
    section,
    file: record.file,
    symbol: record.symbol,
    kind: record.kind,
    scope: section === 'files' ? record.scope : 'core',
    classification: classifyRecord(section, record),
    disposition: record.disposition,
    action: record.action,
    removalGate: record.removalGate,
    finalGate: record.finalGate,
  };
  compact.heartbeatComponent = heartbeatComponent(compact);
  return Object.fromEntries(Object.entries(compact).filter(([, value]) => value !== undefined));
}

function selectedRecords(inventory) {
  const records = [];
  for (const section of allowedSections) {
    if (!Array.isArray(inventory[section])) throw new Error(`inventory section is missing: ${section}`);
    for (const record of inventory[section]) {
      if (section === 'files' && record.scope !== 'core' && record.scope !== 'shared-perf') continue;
      records.push(compactRecord(section, record));
    }
  }
  return records.sort((left, right) => left.id.localeCompare(right.id, 'en'));
}

function countBy(records, selector) {
  const counts = {};
  for (const record of records) {
    const key = selector(record);
    counts[key] = (counts[key] ?? 0) + 1;
  }
  return Object.fromEntries(Object.entries(counts).sort(([left], [right]) => left.localeCompare(right, 'en')));
}

function validate(records, inventory) {
  const findings = [];
  const seen = new Set();
  for (const record of records) {
    if (!record.id || seen.has(record.id)) findings.push(`duplicate-or-empty-id:${record.id ?? '<empty>'}`);
    seen.add(record.id);
    if (!allowedDispositions.has(record.disposition)) findings.push(`unsupported-disposition:${record.id}`);
    if (!record.action || !record.removalGate || !record.finalGate) findings.push(`missing-routing:${record.id}`);
    if (!record.classification) findings.push(`unclassified:${record.id}`);
  }

  const classificationCounts = countBy(records, record => record.classification);
  for (const required of [
    'core-service-remove',
    'zmp-heartbeat-remove',
    'generic-timer-retain-service-fragment-remove',
    'raw-monitor-retain-service-fragment-remove',
    'core-10x-perf-archive-retain',
  ]) {
    if (!classificationCounts[required]) findings.push(`missing-boundary-classification:${required}`);
  }
  const heartbeatCounts = countBy(
    records.filter(record => record.classification === 'zmp-heartbeat-remove'),
    record => record.heartbeatComponent ?? 'unclassified',
  );
  for (const component of ['option', 'frame-codec', 'engine-timer', 'test']) {
    if (!heartbeatCounts[component]) findings.push(`missing-zmp-heartbeat-component:${component}`);
  }
  if (inventory.summary?.unclassifiedServiceFiles !== 0) {
    findings.push(`inventory-unclassified-service-files:${inventory.summary?.unclassifiedServiceFiles ?? '<missing>'}`);
  }
  return {findings, classificationCounts};
}

function buildManifest(inventory, inventoryName) {
  const records = selectedRecords(inventory);
  const {findings, classificationCounts} = validate(records, inventory);
  if (findings.length) throw new Error(`Core readiness validation failed:\n  ${findings.join('\n  ')}`);
  return {
    schema: 1,
    ledgerId: 'V11-M2-CORE-READINESS',
    sourceInventory: inventoryName,
    sourceSelectionSha256: sha256(stableJson(records)),
    policy: {
      recordIdentity: 'exact machine-inventory record id',
      selectedSections: allowedSections,
      selectedFileScopes: ['core', 'shared-perf'],
      inventoryDigestScope: 'Only selected Core and shared-perf records affect sourceSelectionSha256; unrelated binding records do not invalidate this lane.',
      noHitMeaning: 'Every selected inventory record has one removal or preservation classification; unknown disposition, missing routing, and missing boundary classes fail.',
    },
    summary: {
      totalRecords: records.length,
      recordsBySection: countBy(records, record => record.section),
      recordsByClassification: classificationCounts,
      zmpHeartbeatRecordsByComponent: countBy(
        records.filter(record => record.classification === 'zmp-heartbeat-remove'),
        record => record.heartbeatComponent,
      ),
      recordsByDisposition: countBy(records, record => record.disposition),
      unclassifiedRecords: 0,
    },
    records,
  };
}

function compareManifest(expected, actual) {
  const expectedText = stableJson(expected);
  const actualText = stableJson(actual);
  if (expectedText === actualText) return [];
  const expectedIds = new Set(expected.records.map(record => record.id));
  const actualIds = new Set((actual.records ?? []).map(record => record.id));
  const added = [...expectedIds].filter(id => !actualIds.has(id));
  const removed = [...actualIds].filter(id => !expectedIds.has(id));
  const changed = [...expectedIds].filter(id => actualIds.has(id)
    && stableJson(expected.records.find(record => record.id === id))
      !== stableJson(actual.records.find(record => record.id === id)));
  return [
    `manifest differs: expected sha256=${sha256(expectedText)}, actual sha256=${sha256(actualText)}`,
    `new-or-missing-from-manifest=${added.length}`,
    `no-longer-selected=${removed.length}`,
    `classification-changed=${changed.length}`,
  ];
}

function runSelfTest(inventory, inventoryName) {
  const expected = buildManifest(inventory, inventoryName);
  const missingRecord = structuredClone(expected);
  missingRecord.records.pop();
  if (!compareManifest(expected, missingRecord).length) throw new Error('self-test failed to detect a missing record');

  const invalidInventory = structuredClone(inventory);
  invalidInventory.files.push({
    id: 'self-test:unclassified-core-file',
    file: 'core/src/self_test.cpp',
    scope: 'core',
    disposition: 'unclassified',
    action: '',
    removalGate: '',
    finalGate: '',
  });
  let rejected = false;
  try {
    buildManifest(invalidInventory, inventoryName);
  } catch {
    rejected = true;
  }
  if (!rejected) throw new Error('self-test failed to reject an unclassified Core record');
  return 2;
}

function writeEvidence(file, result) {
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, stableJson(result));
}

function git(...arguments_) {
  return childProcess.execFileSync('git', arguments_, {
    cwd: repositoryRoot,
    encoding: 'utf8',
  }).trim();
}

function gitFileContent(revision, file) {
  try {
    return childProcess.execFileSync('git', ['show', `${revision}:${file}`], {
      cwd: repositoryRoot,
      encoding: null,
      stdio: ['ignore', 'pipe', 'ignore'],
    });
  } catch {
    return undefined;
  }
}

function writeCandidateManifests(candidateFile, ownedPathsFile, inventoryName) {
  const baseRevision = git('rev-parse', 'HEAD');
  const files = ownedRepositoryPaths.map(file => {
    const absolute = resolveRepositoryPath(file);
    const content = fs.readFileSync(absolute);
    const baseContent = gitFileContent(baseRevision, file);
    return {
      path: file,
      status: baseContent === undefined ? 'added' : (sha256(content) === sha256(baseContent) ? 'unchanged' : 'modified'),
      mode: (fs.statSync(absolute).mode & 0o111) === 0 ? '100644' : '100755',
      contentSha256: sha256(content),
      baseContentSha256: baseContent === undefined ? null : sha256(baseContent),
    };
  });
  const candidate = {
    schema: 'zlink-v11-ledger-candidate-v1',
    ledgerId: 'V11-M2-CORE-READINESS',
    baseRevision,
    ownedPaths: ownedRepositoryPaths,
    directInputs: [inventoryName],
    pathCount: files.length,
    aggregateSha256: sha256(JSON.stringify(files)),
    files,
  };
  const ownedPaths = {
    schema: 'zlink-v11-owned-paths-v1',
    ledgerId: 'V11-M2-CORE-READINESS',
    ownedPaths: ownedRepositoryPaths,
  };
  writeEvidence(candidateFile, candidate);
  writeEvidence(ownedPathsFile, ownedPaths);
  return {
    candidate: repositoryPath(candidateFile),
    candidateSha256: sha256(fs.readFileSync(candidateFile)),
    ownedPaths: repositoryPath(ownedPathsFile),
    ownedPathsSha256: sha256(fs.readFileSync(ownedPathsFile)),
  };
}

function main() {
  const options = parseArguments(process.argv.slice(2));
  const inventoryFile = resolveRepositoryPath(options.inventory);
  const manifestFile = resolveRepositoryPath(options.manifest);
  const inventoryText = fs.readFileSync(inventoryFile, 'utf8');
  const inventory = JSON.parse(inventoryText);
  const inventoryName = repositoryPath(inventoryFile);
  const expected = buildManifest(inventory, inventoryName);
  const selfTestMutations = options.selfTest
    ? runSelfTest(inventory, inventoryName)
    : 0;

  if (options.mode === 'write') {
    fs.mkdirSync(path.dirname(manifestFile), {recursive: true});
    fs.writeFileSync(manifestFile, stableJson(expected));
  } else {
    if (!fs.existsSync(manifestFile)) throw new Error(`manifest does not exist: ${manifestFile}`);
    const findings = compareManifest(expected, readJson(manifestFile));
    if (findings.length) throw new Error(`Core removal manifest check failed:\n  ${findings.join('\n  ')}`);
  }

  const manifestText = fs.readFileSync(manifestFile, 'utf8');
  const candidate = writeCandidateManifests(
    resolveRepositoryPath(options.candidate),
    resolveRepositoryPath(options.ownedPaths),
    inventoryName,
  );
  const result = {
    schema: 1,
    ledgerId: 'V11-M2-CORE-READINESS',
    status: 'pass',
    mode: options.mode,
    inventory: inventoryName,
    inventorySha256: sha256(inventoryText),
    manifest: repositoryPath(manifestFile),
    manifestSha256: sha256(manifestText),
    candidate,
    summary: expected.summary,
    probes: {
      inventoryUnclassifiedServiceFiles: inventory.summary.unclassifiedServiceFiles,
      selectedRecordCoveragePercent: 100,
      unclassifiedSelectedRecords: 0,
      genericTimerBoundaryRecords: expected.summary.recordsByClassification['generic-timer-retain-service-fragment-remove'],
      rawMonitorBoundaryRecords: expected.summary.recordsByClassification['raw-monitor-retain-service-fragment-remove'],
      negativeMutations: selfTestMutations,
      zmpHeartbeatComponents: expected.summary.zmpHeartbeatRecordsByComponent,
    },
  };
  if (options.evidence) writeEvidence(resolveRepositoryPath(options.evidence), result);
  process.stdout.write(`${stableJson(result)}`);
}

try {
  main();
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
}
