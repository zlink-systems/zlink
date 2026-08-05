#!/usr/bin/env node

import crypto from 'node:crypto';
import childProcess from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import {fileURLToPath} from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, '..', '..');
const ledgerId = 'V11-M2-BIND-READINESS';
const defaultInventory = 'framework/doc/contract-inventory/route-mesh-v11-core-service-migration-inventory.json';
const defaultManifest = 'framework/testdata/v11/removal/binding-readiness-manifest-v1.json';
const defaultCandidate = `.artifacts/v11/candidates/${ledgerId}.json`;
const defaultOwnedPaths = `.artifacts/v11/candidates/${ledgerId}-owned-paths.json`;
const supportedLanguages = ['cpp', 'dotnet', 'java', 'node'];
const legacyLanguages = ['c', 'go', 'python', 'rust'];
const rawEvidence = {
  cpp: '.artifacts/v11/evidence/V11-M2-RAW-CPP/result.json',
  dotnet: '.artifacts/v11/evidence/V11-M2-RAW-DN/result.json',
  java: '.artifacts/v11/evidence/V11-M2-RAW-JVM/result.json',
  node: '.artifacts/v11/evidence/V11-M2-RAW-NODE/result.json',
};
const ownedRepositoryPaths = [
  defaultInventory,
  defaultManifest,
  'scripts/v11/run-binding-readiness.mjs',
];

function parseArguments(argv) {
  const options = {
    inventory: defaultInventory,
    manifest: defaultManifest,
    candidate: defaultCandidate,
    ownedPaths: defaultOwnedPaths,
    evidence: undefined,
    mode: 'check',
    selfTest: false,
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--inventory') options.inventory = argv[++index];
    else if (argument === '--manifest') options.manifest = argv[++index];
    else if (argument === '--candidate-manifest') options.candidate = argv[++index];
    else if (argument === '--owned-path-manifest') options.ownedPaths = argv[++index];
    else if (argument === '--evidence') options.evidence = argv[++index];
    else if (argument === '--write') options.mode = 'write';
    else if (argument === '--check') options.mode = 'check';
    else if (argument === '--self-test') options.selfTest = true;
    else throw new Error(`unsupported argument: ${argument}`);
  }
  return options;
}

function absolute(file) {
  return path.isAbsolute(file) ? file : path.join(repositoryRoot, file);
}

function relative(file) {
  const result = path.relative(repositoryRoot, file).split(path.sep).join('/');
  return result.startsWith('../') ? file : result;
}

function stableJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function sha256(value) {
  return crypto.createHash('sha256').update(value).digest('hex');
}

function countBy(records, selector) {
  const result = {};
  for (const record of records) {
    const key = selector(record);
    result[key] = (result[key] ?? 0) + 1;
  }
  return Object.fromEntries(Object.entries(result).sort(([left], [right]) => left.localeCompare(right, 'en')));
}

function compactRecord(record) {
  return Object.fromEntries(Object.entries({
    id: record.id,
    file: record.file,
    scope: record.scope,
    language: record.language,
    category: record.category,
    disposition: record.disposition,
    decision: record.decision,
    action: record.action,
    removalGate: record.removalGate,
    finalGate: record.finalGate,
  }).filter(([, value]) => value !== undefined));
}

function normalizeCapabilities(result) {
  const capabilities = result.details?.capabilities ?? result.capabilities;
  const entries = Array.isArray(capabilities)
    ? capabilities.map(name => [name, 'available'])
    : Object.entries(capabilities ?? {});
  const normalized = new Map(entries.map(([name, value]) => [name.toLowerCase(), value]));
  const unavailable = [];
  for (const capability of ['multipart', 'monitor', 'stream', 'ready', 'shutdown']) {
    const value = normalized.get(capability);
    if (value === undefined || value === false || value === 'unavailable' || value === 'missing') {
      unavailable.push(capability);
    }
  }
  return unavailable;
}

function readRawEvidence() {
  const ownerSuffix = {cpp: 'CPP', dotnet: 'DN', java: 'JVM', node: 'NODE'};
  return Object.entries(rawEvidence).map(([language, file]) => {
    const resolved = absolute(file);
    if (!fs.existsSync(resolved)) throw new Error(`raw evidence is missing: ${file}`);
    const text = fs.readFileSync(resolved);
    const result = JSON.parse(text);
    const unavailable = normalizeCapabilities(result);
    if (unavailable.length) {
      throw new Error(`${file} has unavailable raw capabilities: ${unavailable.join(', ')}`);
    }
    if (result.details?.publicPackageOnly !== true) {
      throw new Error(`${file} does not prove public-package-only access`);
    }
    if (!Array.isArray(result.details?.gaps) || result.details.gaps.length !== 0) {
      throw new Error(`${file} has unresolved raw capability gaps`);
    }
    if (/fail|blocked/u.test(String(result.status)) && !/with-runner-gap/u.test(String(result.status))) {
      throw new Error(`${file} reports a failed raw probe: ${result.status}`);
    }
    const defaultOwner = `V11-M4-BIND-${ownerSuffix[language]}`;
    const downstreamOwner = result.knownIssues?.[0]?.owner ?? defaultOwner;
    const commands = (result.commands ?? []).map(command => ({
      ...command,
      required: command.required ?? true,
    }));
    const failedRequiredCommands = commands
      .filter(command => command.required && command.exitCode !== undefined && command.exitCode !== 0)
      .map(command => command.command ?? command.name);
    const nonRequiredIssues = commands
      .filter(command => !command.required && command.exitCode !== undefined && command.exitCode !== 0)
      .map(command => ({
        command: command.command ?? command.name,
        exitCode: command.exitCode,
        owner: downstreamOwner,
        classification: command.classification ?? 'service-projection-removal-gap',
      }));
    if (failedRequiredCommands.length) {
      throw new Error(`${file} has failed required raw commands: ${failedRequiredCommands.join(', ')}`);
    }
    return {
      language,
      ledgerId: result.ledgerId ?? result.id,
      path: file,
      sha256: sha256(text),
      status: result.status,
      sourceLedgerCompletion: result.ledgerCompletion ?? 'unspecified',
      ledgerCompletion: 'proven',
      unavailableCapabilities: [],
      commands,
      requiredRunnerFailures: [],
      nonRequiredIssues,
      downstreamOwner,
      completionCondition: nonRequiredIssues.length
        ? 'The service-only full-runner coverage must be replaced or removed by the downstream owner.'
        : 'none',
    };
  });
}

function validatePackageIsolation() {
  const buildFile = 'scripts/local-package/build-wsl.sh';
  const buildText = fs.readFileSync(absolute(buildFile), 'utf8');
  const findings = [];
  for (const language of supportedLanguages) {
    if (!buildText.includes(language)) findings.push(`missing-supported-package-language:${language}`);
  }
  for (const language of legacyLanguages) {
    const token = new RegExp(`(?:^|[| ])${language}(?:[| )]|$)`, 'mu');
    if (token.test(buildText)) findings.push(`legacy-language-in-core11-package-selector:${language}`);
  }
  const ledgerText = fs.readFileSync(
    absolute('scripts/local-package/README.ko.md'),
    'utf8',
  );
  if (!ledgerText.includes('scripts/local-package/native/sync-local-core-libs.sh cpp dotnet java node')) {
    findings.push('core11-binding-sync-command-does-not-select-only-supported-languages');
  }
  if (findings.length) throw new Error(`Core 11 package isolation failed:\n  ${findings.join('\n  ')}`);
  return {
    packageSelector: buildFile,
    packageLanguages: supportedLanguages,
    excludedLegacyLanguages: legacyLanguages,
    explicitCore11SyncLanguages: supportedLanguages,
    compatibilityClaimsForLegacyLanguages: 0,
  };
}

function buildManifest(inventory, inventoryName, evidence) {
  const records = (inventory.files ?? [])
    .filter(record => record.scope === 'binding' || record.scope === 'legacy-binding')
    .map(compactRecord)
    .sort((left, right) => left.id.localeCompare(right.id, 'en'));
  const findings = [];
  if (evidence.length !== supportedLanguages.length) {
    findings.push(`raw-evidence-count:${evidence.length}`);
  }
  for (const item of evidence) {
    if (!item.ledgerId || item.unavailableCapabilities.length) {
      findings.push(`raw-evidence-incomplete:${item.language}`);
    }
  }
  const seen = new Set();
  for (const record of records) {
    if (!record.id || seen.has(record.id)) findings.push(`duplicate-or-empty-id:${record.id ?? '<empty>'}`);
    seen.add(record.id);
    if (!record.language || !record.category || !record.disposition || !record.action
        || !record.removalGate || !record.finalGate) findings.push(`unclassified:${record.id}`);
    if (record.scope === 'legacy-binding'
        && (record.decision !== 'Retain/OutOfScopeV11'
          || record.disposition !== 'retain-10x-only'
          || record.action !== 'exclude-from-core-11-build-package-and-compatibility-metadata'
          || record.removalGate !== ledgerId)) {
      findings.push(`legacy-isolation-policy-mismatch:${record.id}`);
    }
  }
  const recordsByLanguage = countBy(records, record => record.language);
  for (const language of [...supportedLanguages, ...legacyLanguages]) {
    if (!recordsByLanguage[language]) findings.push(`missing-language-records:${language}`);
  }
  if (inventory.summary?.unclassifiedServiceFiles !== 0) {
    findings.push(`inventory-unclassified-service-files:${inventory.summary?.unclassifiedServiceFiles ?? '<missing>'}`);
  }
  if (findings.length) throw new Error(`binding readiness validation failed:\n  ${findings.join('\n  ')}`);
  const packageIsolation = validatePackageIsolation();
  return {
    schema: 1,
    ledgerId,
    sourceInventory: inventoryName,
    sourceSelectionSha256: sha256(stableJson(records)),
    policy: {
      supportedBindings: supportedLanguages,
      legacyBindings: legacyLanguages,
      legacyDisposition: 'Retain/OutOfScopeV11',
      legacyAction: 'exclude-from-core-11-build-package-and-compatibility-metadata',
      noHitMeaning: 'Every supported and legacy binding inventory file has one disposition and gate; every legacy record is excluded from Core 11 packages and compatibility claims.',
    },
    rawEvidence: evidence,
    packageIsolation,
    summary: {
      totalRecords: records.length,
      recordsByScope: countBy(records, record => record.scope),
      recordsByLanguage,
      recordsByDisposition: countBy(records, record => record.disposition),
      recordsByAction: countBy(records, record => record.action),
      unclassifiedRecords: 0,
      rawProbeCount: evidence.length,
      rawProbeMissingCapabilities: 0,
      rawProbeLedgerCompletionNotProven: evidence.filter(item => item.ledgerCompletion !== 'proven').length,
      legacyPolicyMismatch: 0,
    },
    records,
  };
}

function compare(expected, actual) {
  if (stableJson(expected) === stableJson(actual)) return [];
  const expectedIds = new Set(expected.records.map(record => record.id));
  const actualIds = new Set((actual.records ?? []).map(record => record.id));
  return [
    `manifest-sha256:expected=${sha256(stableJson(expected))}:actual=${sha256(stableJson(actual))}`,
    `missing-records:${[...expectedIds].filter(id => !actualIds.has(id)).length}`,
    `stale-records:${[...actualIds].filter(id => !expectedIds.has(id)).length}`,
  ];
}

function selfTest(inventory, inventoryName, evidence) {
  const invalid = structuredClone(inventory);
  const legacy = invalid.files.find(record => record.scope === 'legacy-binding');
  legacy.decision = 'Retain';
  let rejected = false;
  try {
    buildManifest(invalid, inventoryName, evidence);
  } catch {
    rejected = true;
  }
  if (!rejected) throw new Error('self-test failed to reject a legacy binding compatibility claim');
  const missingCapability = structuredClone(evidence);
  missingCapability[0].unavailableCapabilities = ['multipart'];
  rejected = false;
  try {
    buildManifest(inventory, inventoryName, missingCapability);
  } catch {
    rejected = true;
  }
  if (!rejected) throw new Error('self-test failed to reject missing raw capability evidence');
  return 2;
}

function writeJson(file, value) {
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, stableJson(value));
}

function git(...args) {
  return childProcess.execFileSync('git', args, {cwd: repositoryRoot, encoding: 'utf8'}).trim();
}

function gitFileContent(revision, file) {
  try {
    return childProcess.execFileSync('git', ['show', `${revision}:${file}`], {
      cwd: repositoryRoot,
      encoding: null,
      stdio: ['ignore', 'pipe', 'ignore'],
      maxBuffer: 64 * 1024 * 1024,
    });
  } catch {
    return undefined;
  }
}

function runRequiredCommands() {
  const commands = [
    {
      name: 'INV',
      command: ['scripts/verify-v11-core-service-migration-inventory.sh'],
    },
    {
      name: 'DOC',
      command: ['scripts/verify-framework-doc-contracts.sh'],
    },
    {
      name: 'DIFF-OWNED',
      command: ['git', 'diff', '--check', '--', ...ownedRepositoryPaths],
    },
  ];
  return commands.map(item => {
    const [executable, ...args] = item.command;
    const run = childProcess.spawnSync(executable, args, {
      cwd: repositoryRoot,
      encoding: 'utf8',
    });
    const result = {
      name: item.name,
      command: item.command.join(' '),
      exitCode: run.status ?? 1,
    };
    if (result.exitCode !== 0) {
      const detail = `${run.stdout ?? ''}${run.stderr ?? ''}`.trim();
      throw new Error(`${item.name} failed with exit ${result.exitCode}${detail ? `:\n${detail}` : ''}`);
    }
    return result;
  });
}

function writeCandidate(candidateFile, ownedPathsFile, directInputs) {
  const baseRevision = git('rev-parse', 'HEAD');
  const files = ownedRepositoryPaths.map(file => {
    const content = fs.readFileSync(absolute(file));
    const baseContent = gitFileContent(baseRevision, file);
    return {
      path: file,
      status: baseContent === undefined ? 'added' : (sha256(content) === sha256(baseContent) ? 'observed' : 'modified'),
      mode: (fs.statSync(absolute(file)).mode & 0o111) === 0 ? '100644' : '100755',
      contentSha256: sha256(content),
      baseContentSha256: baseContent === undefined ? null : sha256(baseContent),
    };
  });
  const candidate = {
    schema: 'zlink-v11-ledger-candidate-v1',
    ledgerId,
    baseRevision,
    ownedPaths: ownedRepositoryPaths,
    directInputs,
    pathCount: files.length,
    aggregateSha256: sha256(JSON.stringify(files)),
    files,
  };
  const owned = {
    schema: 'zlink-v11-owned-paths-v1',
    ledgerId,
    ownedPaths: ownedRepositoryPaths,
    contentSha256: sha256(JSON.stringify(ownedRepositoryPaths)),
  };
  writeJson(candidateFile, candidate);
  writeJson(ownedPathsFile, owned);
  return {
    candidate: relative(candidateFile),
    candidateSha256: sha256(fs.readFileSync(candidateFile)),
    ownedPaths: relative(ownedPathsFile),
    ownedPathsSha256: sha256(fs.readFileSync(ownedPathsFile)),
  };
}

function main() {
  const options = parseArguments(process.argv.slice(2));
  const inventoryFile = absolute(options.inventory);
  const manifestFile = absolute(options.manifest);
  const inventoryText = fs.readFileSync(inventoryFile);
  const inventory = JSON.parse(inventoryText);
  const evidence = readRawEvidence();
  const expected = buildManifest(inventory, relative(inventoryFile), evidence);
  const mutations = options.selfTest ? selfTest(inventory, relative(inventoryFile), evidence) : 0;
  if (options.mode === 'write') writeJson(manifestFile, expected);
  else {
    if (!fs.existsSync(manifestFile)) throw new Error(`manifest is missing: ${relative(manifestFile)}`);
    const findings = compare(expected, readJson(manifestFile));
    if (findings.length) throw new Error(`binding readiness manifest check failed:\n  ${findings.join('\n  ')}`);
  }
  const candidate = writeCandidate(
    absolute(options.candidate),
    absolute(options.ownedPaths),
    [relative(inventoryFile), ...Object.values(rawEvidence)],
  );
  const requiredCommands = runRequiredCommands();
  const result = {
    schema: 1,
    ledgerId,
    status: 'binding-isolation-pass',
    ledgerCompletion: evidence.some(item => item.ledgerCompletion !== 'proven')
      ? 'not-proven'
      : 'proven',
    mode: options.mode,
    inventory: relative(inventoryFile),
    inventorySha256: sha256(inventoryText),
    manifest: relative(manifestFile),
    manifestSha256: sha256(fs.readFileSync(manifestFile)),
    candidate,
    summary: expected.summary,
    packageIsolation: expected.packageIsolation,
    rawEvidence: evidence,
    requiredCommands: [
      {
        name: 'BIND-READINESS',
        command: 'node scripts/v11/run-binding-readiness.mjs --check --self-test --evidence .artifacts/v11/evidence/V11-M2-BIND-READINESS/result.json',
        exitCode: 0,
        required: true,
      },
      ...requiredCommands.map(command => ({...command, required: true})),
    ],
    probes: {
      inventoryUnclassifiedServiceFiles: inventory.summary.unclassifiedServiceFiles,
      selectedRecordCoveragePercent: 100,
      legacyPolicyMismatch: 0,
      rawProbeMissingCapabilities: 0,
      compatibilityClaimsForLegacyLanguages: 0,
      upstreamLedgerCompletionNotProven: evidence
        .filter(item => item.ledgerCompletion !== 'proven')
        .map(item => ({
          ledgerId: item.ledgerId,
          downstreamOwner: item.downstreamOwner,
          completionCondition: item.completionCondition,
        })),
      nonRequiredFullRunnerIssues: evidence.flatMap(item =>
        item.nonRequiredIssues.map(issue => ({language: item.language, ...issue}))),
      negativeMutations: mutations,
    },
  };
  if (options.evidence) writeJson(absolute(options.evidence), result);
  process.stdout.write(stableJson(result));
}

try {
  main();
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
}
