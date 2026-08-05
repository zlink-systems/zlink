#!/usr/bin/env node

// SPDX-License-Identifier: MPL-2.0

import {spawnSync} from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {
  closedCatchAllExpectations,
  removedMemberBehavior,
  removedMemberParityKey,
  replacementParitySignature,
  semanticMemberKey,
  sourceJvmParityExpectation,
} from './contract-amendment-impact-policy.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const traceRelativePath =
  'framework/doc/contract-inventory/route-mesh-v11-public-contract-trace.json';
const tracePath = path.join(root, traceRelativePath);
const tracePolicyPath = path.join(
  root,
  'framework/doc/contract-inventory/route-mesh-v11-public-contract-trace.config.json',
);
const stableJson = value => `${JSON.stringify(value, null, 2)}\n`;
const sha256 = value => crypto.createHash('sha256').update(value).digest('hex');
const currentHashCache = new Map();
const revisionHashCache = new Map();

function git(args, encoding = 'utf8') {
  const result = spawnSync('git', args, {cwd: root, encoding, maxBuffer: 128 * 1024 * 1024});
  if (result.status !== 0) throw new Error(`git ${args.join(' ')} failed: ${String(result.stderr).trim()}`);
  return result.stdout;
}

function currentHash(relative) {
  if (currentHashCache.has(relative)) return currentHashCache.get(relative);
  const index = git(['ls-files', '-s', '-z', '--', relative], null).toString('utf8');
  const recordsByPath = new Map(index.split('\0').filter(Boolean).map(record => {
    const match = /^(\d+) ([0-9a-f]+) \d+\t(.+)$/u.exec(record);
    if (!match) throw new Error(`unexpected git ls-files record: ${record}`);
    return [match[3], {path: match[3], blob: match[2]}];
  }));
  const changed = git(['diff', '--name-only', '-z', '--', relative], null).toString('utf8')
    .split('\0').filter(Boolean);
  const untracked = git(['ls-files', '--others', '--exclude-standard', '-z', '--', relative], null)
    .toString('utf8').split('\0').filter(Boolean);
  for (const file of [...changed, ...untracked]) {
    const absolute = path.join(root, file);
    if (!fs.existsSync(absolute) || !fs.statSync(absolute).isFile()) {
      recordsByPath.delete(file);
      continue;
    }
    const object = git(['hash-object', `--path=${file}`, file]).trim();
    recordsByPath.set(file, {path: file, blob: object});
  }
  const records = [...recordsByPath.values()]
    .sort((left, right) => left.path.localeCompare(right.path, 'en'));
  const result = records.length === 0 ? null : sha256(stableJson(records));
  currentHashCache.set(relative, result);
  return result;
}

function revisionHash(revision, relative) {
  const cacheKey = `${revision}\0${relative}`;
  if (revisionHashCache.has(cacheKey)) return revisionHashCache.get(cacheKey);
  const output = git(['ls-tree', '-r', '--full-tree', revision, '--', relative]);
  const records = output.trim().length === 0 ? [] : output.trim().split('\n').map(record => {
    const match = /^(\d+) blob ([0-9a-f]+)\t(.+)$/u.exec(record);
    if (!match) throw new Error(`unexpected git ls-tree record: ${record}`);
    return {path: match[3], blob: match[2]};
  }).sort((left, right) => left.path.localeCompare(right.path, 'en'));
  const result = records.length === 0 ? null : sha256(stableJson(records));
  revisionHashCache.set(cacheKey, result);
  return result;
}

function revisionFile(revision, relative) {
  return git(['show', `${revision}:${relative}`]);
}

function currentTrackedFiles(relative) {
  return git(['ls-files', '-z', '--', relative], null).toString('utf8')
    .split('\0').filter(Boolean)
    .filter(relativePath => fs.existsSync(path.join(root, relativePath))
      && fs.statSync(path.join(root, relativePath)).isFile())
    .sort((left, right) => left.localeCompare(right, 'en'));
}

const requiredRawRegressionRoots = [
  {scope: 'core', language: 'core', path: 'core/tests', runtimeOwner: 'V11-M3-CORE-VERIFY'},
  {scope: 'binding', language: 'cpp', path: 'bindings/cpp/tests', runtimeOwner: 'V11-M4-BIND-CPP'},
  {scope: 'binding', language: 'dotnet', path: 'bindings/dotnet/tests', runtimeOwner: 'V11-M4-BIND-DN'},
  {scope: 'binding', language: 'java', path: 'bindings/java/src/test', runtimeOwner: 'V11-M4-BIND-JVM'},
  {scope: 'binding', language: 'node', path: 'bindings/node/tests', runtimeOwner: 'V11-M4-BIND-NODE'},
];

function validateManifest(manifest, mode, {checkFiles = true} = {}) {
  const errors = [];
  const fail = message => errors.push(message);
  if (manifest?.schemaVersion !== 1 || manifest?.version !== '11.0.0') {
    fail('manifest schemaVersion/version must identify RouteMesh 11.0.0 schema 1');
  }
  if (manifest?.baselineRevision !== '1f5b979675c4ece4bd9e126d1a66653157ac3b52') {
    fail('manifest baselineRevision must identify the reviewed M5 amendment base');
  }
  if (!['quarantine', 'finalized'].includes(mode)) fail(`unknown mode ${mode}`);
  if (manifest?.execution?.executed !== 0 || manifest?.execution?.skipped !== 0) {
    fail('impact manifest execution counters must remain executed=0 and skipped=0 before activation');
  }
  if (!Array.isArray(manifest?.entries) || manifest.entries.length === 0) {
    fail('manifest entries must be a non-empty array');
    return errors;
  }
  const tracePolicy = JSON.parse(fs.readFileSync(tracePolicyPath, 'utf8'));
  const runtimeOwnerIds = new Set([
    'V11-M3-CORE-VERIFY', 'V11-M4-BIND-CPP', 'V11-M4-BIND-DN',
    'V11-M4-BIND-JVM', 'V11-M4-BIND-NODE', 'V11-M6-SCAFFOLD-ZERO',
    'V11-CA-PROTOCOL', 'V11-M7-CONTRACT', 'V11-M7-SAMPLES',
  ]);
  for (const category of tracePolicy.categories ?? []) {
    for (const owner of category.contractTestOwners ?? []) runtimeOwnerIds.add(owner);
    for (const stage of category.implementationStages ?? []) {
      for (const suffix of ['CPP', 'DN', 'JVM', 'NODE']) {
        runtimeOwnerIds.add(stage.replace('{suffix}', suffix));
      }
    }
  }
  const ids = new Set();
  const allowedDispositions = new Set(['retain', 'amend', 'replace', 'add', 'remove']);
  const allowedQuarantine = new Set([
    'pending-disabled-by-contract-amendment', 'pending-disabled-reviewed-source',
    'active-contract-spec',
    'active-regression', 'planned-regression',
  ]);
  for (const entry of manifest.entries) {
    const label = entry?.id ?? '<missing-id>';
    if (typeof entry?.id !== 'string' || entry.id.length === 0 || ids.has(entry.id)) {
      fail(`${label}: stable ID is missing or duplicated`);
    }
    ids.add(entry?.id);
    if (!allowedDispositions.has(entry?.disposition)) fail(`${label}: invalid disposition`);
    if (!allowedQuarantine.has(entry?.quarantineStatus)) fail(`${label}: invalid quarantineStatus`);
    for (const field of ['kind', 'language', 'acceptanceIntent', 'specOwner', 'runtimeOwner', 'activationStage']) {
      if (typeof entry?.[field] !== 'string' || entry[field].trim().length === 0) {
        fail(`${label}: ${field} must be a non-empty string`);
      }
    }
    if (!runtimeOwnerIds.has(entry.runtimeOwner)) {
      fail(`${label}: runtimeOwner is not a current trace-policy owner`);
    }
    const ownerPath = path.join(root, entry.specOwner ?? '');
    if (!fs.existsSync(ownerPath) || !fs.statSync(ownerPath).isFile()) fail(`${label}: specOwner does not exist`);
    if (!Array.isArray(entry.replacementCoverage)) fail(`${label}: replacementCoverage must be an array`);
    if (entry.disposition === 'remove' && entry.replacementCoverage.length === 0) {
      fail(`${label}: remove requires replacement coverage`);
    }
    if (entry.disposition === 'add') {
      if (entry.baselineHash !== null) fail(`${label}: add must not have a baselineHash`);
    } else if (!/^[0-9a-f]{64}$/u.test(entry.baselineHash ?? '')) {
      fail(`${label}: baselineHash must be SHA-256`);
    }
    if (entry.baselinePath !== undefined) {
      if (typeof entry.baselinePath !== 'string' || entry.baselinePath.length === 0) {
        fail(`${label}: baselinePath must be a non-empty repository-relative path`);
      } else if (revisionHash(manifest.baselineRevision, entry.baselinePath) !== entry.baselineHash) {
        fail(`${label}: baselinePath hash does not match baselineHash`);
      }
    }

    if (mode === 'quarantine') {
      if (entry.quarantineStatus === 'pending-disabled-by-contract-amendment'
          && entry.approvedHash !== null) {
        fail(`${label}: quarantined E2E/sample item must not have an approvedHash before finalization`);
      }
      if (entry.quarantineStatus === 'pending-disabled-reviewed-source'
          && !/^[0-9a-f]{64}$/u.test(entry.approvedHash ?? '')) {
        fail(`${label}: reviewed but execution-disabled source requires an approvedHash`);
      }
    } else if (['amend', 'replace', 'add'].includes(entry.disposition)
        && !/^[0-9a-f]{64}$/u.test(entry.approvedHash ?? '')) {
      fail(`${label}: finalized changed item requires approvedHash`);
    }

    if (!checkFiles || entry.path === null) {
      if (mode === 'finalized' && entry.disposition !== 'remove'
          && entry.kind !== 'raw-regression-test') {
        fail(`${label}: finalized item requires a path`);
      }
      continue;
    }
    if (typeof entry.path !== 'string' || entry.path.startsWith('/') || entry.path.includes('..')) {
      fail(`${label}: path must be repository-relative`);
      continue;
    }
    const hash = currentHash(entry.path);
    if (hash === null) {
      if (entry.disposition !== 'remove') fail(`${label}: current path is missing`);
      continue;
    }
    if (mode === 'quarantine' && ![
      'pending-disabled-by-contract-amendment',
      'pending-disabled-reviewed-source',
    ].includes(entry.quarantineStatus)) {
      continue;
    }
    const expected = entry.quarantineStatus === 'pending-disabled-reviewed-source'
      ? entry.approvedHash
      : mode === 'finalized' && entry.approvedHash
        ? entry.approvedHash
        : entry.baselineHash;
    if (expected && hash !== expected) {
      fail(`${label}: current hash differs from ${mode} approved baseline expected=${expected} actual=${hash}`);
    }
  }
  for (const entry of manifest.entries) {
    for (const replacement of entry.replacementCoverage ?? []) {
      if (!ids.has(replacement)) fail(`${entry.id}: replacement coverage does not resolve: ${replacement}`);
    }
  }

  const baselineTraceSource = revisionFile(manifest.baselineRevision, traceRelativePath);
  const currentTraceSource = fs.readFileSync(tracePath, 'utf8');
  const baselineTrace = JSON.parse(baselineTraceSource);
  const currentTrace = JSON.parse(currentTraceSource);
  const baselineIdentities = new Set(baselineTrace.members.map(member => member.identity));
  const currentIdentities = new Set(currentTrace.members.map(member => member.identity));
  const expectedAdded = currentTrace.members.filter(member => !baselineIdentities.has(member.identity));
  const expectedRemoved = baselineTrace.members.filter(member => !currentIdentities.has(member.identity));
  const publicMemberId = (disposition, member) =>
    `public-member:${disposition}:${member.language}:${sha256(member.identity).slice(0, 20)}`;
  const additionsBySemanticMember = new Map();
  for (const member of expectedAdded) {
    const key = semanticMemberKey(member);
    if (!additionsBySemanticMember.has(key)) additionsBySemanticMember.set(key, []);
    additionsBySemanticMember.get(key).push(member);
  }
  const expectedSignatureReplacementCount = expectedRemoved.filter(member =>
    additionsBySemanticMember.get(semanticMemberKey(member))?.length).length;
  const expectedBehaviorReplacementCount = expectedRemoved.length - expectedSignatureReplacementCount;
  const behaviorByIdentity = new Map();
  const parityGroups = new Map();
  for (const member of expectedRemoved) {
    if (additionsBySemanticMember.get(semanticMemberKey(member))?.length) continue;
    const behavior = removedMemberBehavior(member);
    behaviorByIdentity.set(member.identity, behavior);
    const key = removedMemberParityKey(member);
    if (!parityGroups.has(key)) parityGroups.set(key, []);
    parityGroups.get(key).push({member, behavior});
  }
  const crossLanguageParityGroups = [...parityGroups.entries()].filter(([, group]) =>
    new Set(group.map(item => item.member.language)).size > 1);
  const sourceJvmParityGroups = [...parityGroups.entries()].filter(([, group]) =>
    group.every(item => item.member.language === 'kotlin')
      && new Set(group.map(item => item.member.ownerIdentity)).size > 1);
  const sourceJvmIdentities = sourceJvmParityGroups.flatMap(([, group]) =>
    group.map(item => item.member.identity)).sort((left, right) => left.localeCompare(right, 'en'));
  const sourceJvmIdentitySetSha256 = sha256(stableJson(sourceJvmIdentities));
  if (sourceJvmParityGroups.length !== sourceJvmParityExpectation.groups
      || sourceJvmIdentitySetSha256 !== sourceJvmParityExpectation.identitySetSha256) {
    fail('Kotlin source/JVM parity identity set differs from reviewed seal');
  }
  const auditedParityGroups = [...new Map([
    ...crossLanguageParityGroups,
    ...sourceJvmParityGroups,
  ]).entries()];
  const policyParityMismatches = auditedParityGroups.filter(([, group]) =>
    new Set(group.map(item => replacementParitySignature(item.behavior))).size > 1);
  if (policyParityMismatches.length > 0) {
    fail(`reviewed removal policy has language parity mismatches: ${policyParityMismatches.map(([key]) => key).join(', ')}`);
  }
  const expectedBehaviorRuleCounts = Object.fromEntries([...behaviorByIdentity.values()]
    .reduce((counts, behavior) => {
      counts.set(behavior.ruleId, (counts.get(behavior.ruleId) ?? 0) + 1);
      return counts;
    }, new Map()).entries().toArray()
    .sort(([left], [right]) => left.localeCompare(right, 'en')));
  for (const [ruleId, expected] of Object.entries(closedCatchAllExpectations)) {
    const identities = expectedRemoved.filter(member =>
      behaviorByIdentity.get(member.identity)?.ruleId === ruleId)
      .map(member => member.identity).sort((left, right) => left.localeCompare(right, 'en'));
    if (identities.length !== expected.count
        || sha256(stableJson(identities)) !== expected.identitySetSha256) {
      fail(`closed catch-all identity set differs from reviewed set: ${ruleId}`);
    }
  }
  const traceDelta = manifest.publicContractTraceDelta;
  if (traceDelta?.path !== traceRelativePath
      || traceDelta?.baselineSha256 !== sha256(baselineTraceSource)
      || traceDelta?.currentSha256 !== sha256(currentTraceSource)
      || traceDelta?.added !== expectedAdded.length
      || traceDelta?.removed !== expectedRemoved.length
      || traceDelta?.replacementPolicy?.signature !== expectedSignatureReplacementCount
      || traceDelta?.replacementPolicy?.behavior !== expectedBehaviorReplacementCount
      || traceDelta?.replacementPolicy?.unmatched !== 0
      || traceDelta?.replacementPolicy?.ambiguous !== 0
      || traceDelta?.replacementPolicy?.crossLanguageGroups !== crossLanguageParityGroups.length
      || traceDelta?.replacementPolicy?.sourceJvmGroups !== sourceJvmParityGroups.length
      || traceDelta?.replacementPolicy?.sourceJvmRecoveredPairs
        !== sourceJvmParityExpectation.recoveredPairs
      || traceDelta?.replacementPolicy?.sourceJvmIdentitySetSha256
        !== sourceJvmIdentitySetSha256
      || traceDelta?.replacementPolicy?.auditedParityGroups !== auditedParityGroups.length
      || traceDelta?.replacementPolicy?.parityMismatches !== 0
      || stableJson(traceDelta?.replacementPolicy?.behaviorByRule)
        !== stableJson(expectedBehaviorRuleCounts)
      || stableJson(traceDelta?.replacementPolicy?.closedCatchAll)
        !== stableJson(closedCatchAllExpectations)) {
    fail('publicContractTraceDelta does not match the approved baseline/current trace files');
  }
  const publicEntries = manifest.entries.filter(entry => entry.kind === 'public-member');
  const publicEntryKeys = new Set();
  for (const entry of publicEntries) {
    const key = `${entry.disposition}\0${entry.memberIdentity}`;
    if (publicEntryKeys.has(key)) fail(`${entry.id}: duplicate public trace identity`);
    publicEntryKeys.add(key);
  }
  const assertTraceMember = (member, disposition) => {
    const key = `${disposition}\0${member.identity}`;
    if (!publicEntryKeys.has(key)) fail(`public trace delta is missing ${disposition}: ${member.identity}`);
    const entry = publicEntries.find(candidate =>
      candidate.disposition === disposition && candidate.memberIdentity === member.identity);
    if (!entry) return;
    if (entry.language !== member.language || entry.memberName !== member.memberName
        || entry.ownerIdentity !== member.ownerIdentity) {
      fail(`${entry.id}: public trace member metadata does not match ${member.identity}`);
    }
    const expectedPath = disposition === 'add' ? entry.path : entry.baselinePath;
    if (expectedPath !== member.exactInterface) {
      fail(`${entry.id}: exact interface path does not match trace member`);
    }
    if (disposition === 'remove') {
      const signatureReplacements = additionsBySemanticMember.get(semanticMemberKey(member)) ?? [];
      const behavior = signatureReplacements.length === 0
        ? behaviorByIdentity.get(member.identity)
        : null;
      const expectedReplacementKind = signatureReplacements.length > 0 ? 'signature' : 'behavior';
      const expectedReplacementRule = signatureReplacements.length > 0
        ? 'exact-semantic-member'
        : behavior.ruleId;
      const expectedCoverage = signatureReplacements.length > 0
        ? signatureReplacements.map(replacement => publicMemberId('add', replacement))
        : behavior.replacementCoverage;
      if (entry.replacementKind !== expectedReplacementKind
          || entry.replacementRule !== expectedReplacementRule
          || stableJson([...entry.replacementCoverage].sort())
            !== stableJson([...expectedCoverage].sort())) {
        fail(`${entry.id}: semantic replacement mapping does not match reviewed policy`);
      }
      if (!Array.isArray(entry.decisionCoverage)
          || stableJson([...entry.decisionCoverage].sort())
            !== stableJson([...(behavior?.decisionCoverage ?? [])].sort())) {
        fail(`${entry.id}: CA decision coverage does not match reviewed policy`);
      }
    }
  };
  expectedAdded.forEach(member => assertTraceMember(member, 'add'));
  expectedRemoved.forEach(member => assertTraceMember(member, 'remove'));
  if (publicEntries.length !== expectedAdded.length + expectedRemoved.length) {
    fail('public-member inventory contains entries outside the exact trace delta');
  }
  for (const [key, group] of auditedParityGroups) {
    const actualSignatures = group.map(({member}) => {
      const entry = publicEntries.find(candidate => candidate.disposition === 'remove'
        && candidate.memberIdentity === member.identity);
      return entry ? replacementParitySignature({
        decisionCoverage: entry.decisionCoverage,
        replacementCoverage: entry.replacementCoverage,
      }) : '<missing>';
    });
    if (new Set(actualSignatures).size !== 1) {
      fail(`removed semantic member differs across languages: ${key}`);
    }
  }
  const parityCoverage = manifest.entries.filter(entry => entry.kind === 'public-behavior'
    && entry.id.startsWith('public-behavior:formal-contract-parity:'));
  const expectedParityIds = ['cpp', 'dotnet', 'java', 'kotlin', 'node']
    .map(language => `public-behavior:formal-contract-parity:${language}`).sort();
  if (stableJson(parityCoverage.map(entry => entry.id).sort()) !== stableJson(expectedParityIds)) {
    fail('formal public-contract parity coverage must exist exactly once for all five languages');
  }

  const rawRootEntries = manifest.entries.filter(entry => entry.kind === 'raw-regression-root');
  const rawTestEntries = manifest.entries.filter(entry => entry.kind === 'raw-regression-test');
  for (const required of requiredRawRegressionRoots) {
    const roots = rawRootEntries.filter(entry => entry.scope === required.scope
      && entry.language === required.language && entry.path === required.path
      && entry.runtimeOwner === required.runtimeOwner);
    if (roots.length !== 1) {
      fail(`required raw regression root must appear exactly once: ${required.path}`);
    }
    const expectedFiles = currentTrackedFiles(required.path);
    const actualFiles = rawTestEntries.filter(entry => entry.scope === required.scope
      && entry.language === required.language).map(entry => entry.path)
      .sort((left, right) => left.localeCompare(right, 'en'));
    if (expectedFiles.length === 0 || stableJson(actualFiles) !== stableJson(expectedFiles)) {
      fail(`raw regression file inventory does not match current tracked scope: ${required.path}`);
    }
    for (const entry of rawTestEntries.filter(candidate => candidate.scope === required.scope
      && candidate.language === required.language)) {
      if (currentHash(entry.path) !== entry.baselineHash) {
        fail(`${entry.id}: raw regression snapshot hash does not match current tracked content`);
      }
    }
  }
  if (rawRootEntries.length !== requiredRawRegressionRoots.length) {
    fail('raw regression root inventory contains missing or unapproved scopes');
  }
  const expectedRawFileCount = requiredRawRegressionRoots.reduce(
    (count, required) => count + currentTrackedFiles(required.path).length, 0);
  if (rawTestEntries.length !== expectedRawFileCount) {
    fail('raw regression file inventory contains missing or unapproved entries');
  }
  const individualRegressionLanguages = new Set(manifest.entries
    .filter(entry => entry.kind === 'regression-test')
    .map(entry => entry.language));
  for (const language of ['cpp', 'dotnet', 'java', 'kotlin', 'node', 'common']) {
    if (!individualRegressionLanguages.has(language)) {
      fail(`individual regression-test inventory is missing for ${language}`);
    }
  }
  const plannedRuntimeLanguages = new Set(manifest.entries
    .filter(entry => entry.kind === 'regression' && entry.disposition === 'add')
    .map(entry => entry.language));
  for (const language of ['cpp', 'dotnet', 'java', 'node']) {
    if (!plannedRuntimeLanguages.has(language)) {
      fail(`planned deterministic runtime regression coverage is missing for ${language}`);
    }
  }
  return errors;
}

function selfTest(manifest) {
  const mutations = [
    candidate => { candidate.entries[1].id = candidate.entries[0].id; },
    candidate => { candidate.entries[0].disposition = 'ignore'; },
    candidate => { candidate.execution.executed = 1; },
    candidate => { candidate.entries[0].baselineHash = 'not-a-sha256'; },
    candidate => {
      candidate.entries[0].disposition = 'remove';
      candidate.entries[0].replacementCoverage = [];
    },
    candidate => {
      candidate.entries = candidate.entries.filter(entry => entry.kind !== 'public-member').slice(1);
    },
    candidate => {
      candidate.entries = candidate.entries.filter(entry => entry.kind !== 'raw-regression-test'
        || entry.path !== 'core/tests/CMakeLists.txt');
    },
    candidate => {
      const removal = candidate.entries.find(entry =>
        entry.kind === 'public-member' && entry.replacementKind === 'behavior');
      removal.replacementCoverage = ['e2e:add:not-reviewed'];
    },
    candidate => {
      const removal = candidate.entries.find(entry =>
        entry.kind === 'public-member' && entry.disposition === 'remove');
      removal.decisionCoverage = ['CA-D99'];
    },
    candidate => {
      const removal = candidate.entries.find(entry => entry.kind === 'public-member'
        && entry.disposition === 'remove'
        && /EntrySpotOptions.*routingId|entry_spot_options_t\.routing_id/u.test(entry.memberIdentity));
      removal.decisionCoverage = ['CA-D18', 'CA-D28'];
    },
    candidate => {
      const removals = candidate.entries.filter(entry => entry.kind === 'public-member'
        && entry.disposition === 'remove' && entry.language === 'kotlin'
        && entry.memberName === 'listMeshNodes');
      if (removals.length < 2) throw new Error('source/JVM parity mutation requires listMeshNodes representations');
      removals[0].decisionCoverage = ['CA-D29'];
    },
    candidate => {
      const removal = candidate.entries.find(entry =>
        entry.replacementRule === 'node-reviewed-contract-set');
      removal.replacementRule = 'spot-location-filter';
    },
    candidate => {
      candidate.publicContractTraceDelta.replacementPolicy.sourceJvmRecoveredPairs = 0;
    },
  ];
  for (const [index, mutate] of mutations.entries()) {
    const candidate = structuredClone(manifest);
    mutate(candidate);
    if (validateManifest(candidate, 'quarantine', {checkFiles: false}).length === 0) {
      throw new Error(`impact verifier negative self-test accepted invalid mutation ${index + 1}`);
    }
  }
  const hashCandidate = structuredClone(manifest);
  const hashedEntry = hashCandidate.entries.find(entry =>
    entry.path !== null && entry.disposition !== 'add'
      && entry.quarantineStatus === 'pending-disabled-by-contract-amendment');
  if (!hashedEntry) throw new Error('impact verifier self-test requires one quarantined hashed entry');
  hashedEntry.baselineHash = '0'.repeat(64);
  if (validateManifest(hashCandidate, 'quarantine').length === 0) {
    throw new Error('impact verifier negative self-test accepted a current-content hash mismatch');
  }
  return mutations.length + 1;
}

const args = process.argv.slice(2);
const findArg = name => {
  const index = args.indexOf(name);
  return index < 0 ? null : args[index + 1];
};
const manifestArgument = findArg('--manifest');
if (!manifestArgument) {
  process.stderr.write('usage: verify-contract-amendment-impact.mjs --manifest <path> --mode <quarantine|finalized> [--evidence <path>] [--self-test]\n');
  process.exit(2);
}
const manifestFile = path.resolve(root, manifestArgument);
const mode = findArg('--mode') ?? 'quarantine';
const evidenceArgument = findArg('--evidence');
const manifest = JSON.parse(fs.readFileSync(manifestFile, 'utf8'));
const errors = validateManifest(manifest, mode);
let negativeMutations = 0;
if (args.includes('--self-test')) negativeMutations = selfTest(manifest);
const counts = manifest.entries.reduce((result, entry) => {
  result.byKind[entry.kind] = (result.byKind[entry.kind] ?? 0) + 1;
  result.byDisposition[entry.disposition] = (result.byDisposition[entry.disposition] ?? 0) + 1;
  result.byQuarantineStatus[entry.quarantineStatus] =
    (result.byQuarantineStatus[entry.quarantineStatus] ?? 0) + 1;
  return result;
}, {byKind: {}, byDisposition: {}, byQuarantineStatus: {}});
const result = {
  schemaVersion: 1,
  stage: 'V11-CA-IMPACT',
  mode,
  status: errors.length === 0 ? 'passed' : 'failed',
  manifest: path.relative(root, manifestFile),
  entries: manifest.entries.length,
  ...counts,
  executed: manifest.execution.executed,
  skipped: manifest.execution.skipped,
  negativeMutations,
  errors,
};
if (evidenceArgument) {
  const evidenceFile = path.resolve(evidenceArgument);
  fs.mkdirSync(path.dirname(evidenceFile), {recursive: true});
  fs.writeFileSync(evidenceFile, stableJson(result));
}
if (errors.length > 0) {
  process.stderr.write(`${errors.join('\n')}\n`);
  process.exit(1);
}
process.stdout.write(
  `contract amendment impact ${mode} passed: entries=${manifest.entries.length}`
  + ` pending=${counts.byQuarantineStatus['pending-disabled-by-contract-amendment'] ?? 0}`
  + ` executed=${manifest.execution.executed} skipped=${manifest.execution.skipped}`
  + ` negative_mutations=${negativeMutations}\n`,
);
