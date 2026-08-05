#!/usr/bin/env node

// SPDX-License-Identifier: MPL-2.0

import {spawnSync} from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {
  auditRemovedMemberBehavior,
  closedCatchAllExpectations,
  removedMemberBehavior,
  removedMemberParityKey,
  replacementParitySignature,
  reviewedRegistrationHashes,
  reviewedRegressionReplacements,
  semanticMemberKey,
  sourceJvmParityExpectation,
} from './contract-amendment-impact-policy.mjs';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(scriptDirectory, '../..');
const manifestPath = path.join(
  root,
  'framework/doc/contract-inventory/route-mesh-v11-contract-amendment-impact.json',
);
const traceRelativePath =
  'framework/doc/contract-inventory/route-mesh-v11-public-contract-trace.json';
const tracePath = path.join(root, traceRelativePath);

const git = (args, encoding = 'utf8') => {
  const result = spawnSync('git', args, {cwd: root, encoding, maxBuffer: 128 * 1024 * 1024});
  if (result.status !== 0) {
    throw new Error(`git ${args.join(' ')} failed: ${String(result.stderr).trim()}`);
  }
  return result.stdout;
};

const sha256 = value => crypto.createHash('sha256').update(value).digest('hex');
const stableJson = value => `${JSON.stringify(value, null, 2)}\n`;
// V11 contract-amendment impact is always measured from the reviewed M5 base.
// Using HEAD makes a committed manifest stale as soon as the candidate is committed.
const baselineRevision = '1f5b979675c4ece4bd9e126d1a66653157ac3b52';
git(['cat-file', '-e', `${baselineRevision}^{commit}`]);

function revisionFile(revision, relative) {
  return git(['show', `${revision}:${relative}`]);
}

function baselineFiles(relative) {
  const output = git(['ls-tree', '-r', '--full-tree', baselineRevision, '--', relative]);
  return output.trim().length === 0 ? [] : output.trim().split('\n').map(line => {
    const match = /^(\d+) blob ([0-9a-f]+)\t(.+)$/u.exec(line);
    if (!match) throw new Error(`unexpected git ls-tree record: ${line}`);
    return {object: match[2], path: match[3]};
  });
}

function baselineHash(relative) {
  const records = baselineFiles(relative).map(record => ({
    path: record.path,
    blob: record.object,
  })).sort((left, right) => left.path.localeCompare(right.path, 'en'));
  if (records.length === 0) throw new Error(`baseline path is empty: ${relative}`);
  return sha256(stableJson(records));
}

function currentTrackedFiles(relative) {
  const paths = git(['ls-files', '-z', '--', relative], null).toString('utf8')
    .split('\0').filter(Boolean)
    .filter(relativePath => fs.existsSync(path.join(root, relativePath))
      && fs.statSync(path.join(root, relativePath)).isFile());
  return paths.map(relativePath => ({
    path: relativePath,
    blob: git(['hash-object', `--path=${relativePath}`, relativePath]).trim(),
  })).sort((left, right) => left.path.localeCompare(right.path, 'en'));
}

function currentTrackedHash(relative) {
  const records = currentTrackedFiles(relative);
  if (records.length === 0) throw new Error(`current tracked path is empty: ${relative}`);
  return sha256(stableJson(records));
}

function directDirectories(relative) {
  const prefix = `${relative}/`;
  return [...new Set(baselineFiles(relative).map(record => {
    const remainder = record.path.slice(prefix.length);
    return remainder.split('/')[0];
  }).filter(Boolean))].sort((left, right) => left.localeCompare(right, 'en'));
}

function activationStage(name) {
  const normalized = name.toLowerCase();
  if (/observability|monitoring|storefailure|store-failure|resilience/u.test(normalized)) {
    return 'V11-M6C-E2E';
  }
  if (/spot|actor|instance|automaticturn|automatic-turn|yielddispatch|toactormessaging/u.test(normalized)) {
    return 'V11-M6B-E2E';
  }
  return 'V11-M6A-E2E';
}

function languageOwner(language, stage) {
  const suffix = {cpp: 'CPP', dotnet: 'DN', java: 'JVM', kotlin: 'JVM', node: 'NODE'}[language];
  if (stage === 'V11-M6A-E2E') return `V11-M6A-${suffix}`;
  if (stage === 'V11-M6B-E2E') return `V11-M6B-${suffix}`;
  return `V11-M6C-${suffix}`;
}

function registrationFiles(relative) {
  return baselineFiles(relative).filter(record => {
    const name = path.basename(record.path);
    return /^(?:run|verify|test)[-_].*\.(?:sh|ps1|mjs|js)$/u.test(name)
      || name === 'CMakeLists.txt'
      || name === 'package.json'
      || name === 'pom.xml'
      || /\.csproj$/u.test(name)
      || /^build\.gradle(?:\.kts)?$/u.test(name);
  });
}

function regressionFiles(relative, language) {
  const patterns = {
    cpp: /(?:^|\/)(?:test[^/]*|[^/]*_test)\.cpp$/u,
    dotnet: /(?:^|\/)[^/]*(?:Tests?|Test)\.cs$/u,
    java: /(?:^|\/)[^/]*(?:Tests?|Test)\.java$/u,
    kotlin: /(?:^|\/)[^/]*(?:Tests?|Test)\.kt$/u,
    node: /(?:^|\/)[^/]*(?:test|spec)\.(?:ts|js|mjs)$/u,
    common: /(?:validate|verify|fixture).*\.(?:mjs|json)$/u,
  };
  return baselineFiles(relative).filter(record => patterns[language].test(record.path));
}

const entries = [];
const addBaselineEntry = entry => entries.push({
  ...entry,
  baselineHash: baselineHash(entry.path),
  approvedHash: null,
});
const addExecutionDisabledReviewedSourceEntry = entry => {
  const baseline = baselineHash(entry.path);
  const current = currentTrackedHash(entry.path);
  entries.push({
    ...entry,
    baselineHash: baseline,
    approvedHash: current === baseline ? null : current,
    quarantineStatus: current === baseline
      ? 'pending-disabled-by-contract-amendment'
      : 'pending-disabled-reviewed-source',
  });
};
const addReviewedRegistrationEntry = entry => {
  const approvedHash = reviewedRegistrationHashes[entry.path];
  if (approvedHash === undefined) {
    addBaselineEntry(entry);
    return;
  }
  const current = currentTrackedHash(entry.path);
  if (current !== approvedHash) {
    throw new Error(
      `reviewed registration changed without review: ${entry.path}`
      + ` expected=${approvedHash} actual=${current}`,
    );
  }
  entries.push({
    ...entry,
    baselineHash: baselineHash(entry.path),
    approvedHash,
    quarantineStatus: 'pending-disabled-reviewed-source',
  });
};

const addReviewedRegressionEntry = entry => {
  const replacement = reviewedRegressionReplacements[entry.path];
  if (replacement === undefined) {
    addBaselineEntry(entry);
    return;
  }
  const current = currentTrackedHash(replacement.path);
  if (current !== replacement.approvedHash) {
    throw new Error(
      `reviewed regression replacement changed without review: ${replacement.path}`
      + ` expected=${replacement.approvedHash} actual=${current}`,
    );
  }
  entries.push({
    ...entry,
    path: replacement.path,
    baselinePath: entry.path,
    baselineHash: baselineHash(entry.path),
    approvedHash: replacement.approvedHash,
    quarantineStatus: 'active-regression',
  });
};

const languageRoots = [
  {language: 'cpp', e2e: ['framework/languages/cpp/e2e'], samples: ['framework/languages/cpp/samples']},
  {language: 'dotnet', e2e: ['framework/languages/dotnet/e2e'], samples: ['framework/languages/dotnet/samples']},
  {
    language: 'java',
    e2e: ['framework/languages/java/e2e'],
    samples: ['framework/languages/java/samples/java'],
  },
  {
    language: 'kotlin',
    e2e: ['framework/languages/java/e2e-kotlin'],
    samples: ['framework/languages/java/samples/kotlin'],
  },
  {language: 'node', e2e: ['framework/languages/node/e2e'], samples: ['framework/languages/node/samples']},
];

for (const scope of languageRoots) {
  for (const e2eRoot of scope.e2e) {
    if (baselineFiles(e2eRoot).length === 0) continue;
    for (const suite of directDirectories(e2eRoot)) {
      const stage = activationStage(suite);
      const suitePath = `${e2eRoot}/${suite}`;
      const suiteEntry = {
        id: `e2e:${scope.language}:${suite}`,
        kind: 'e2e-scenario-suite',
        language: scope.language,
        path: suitePath,
        disposition: 'amend',
        acceptanceIntent: '기존 scenario의 검증 의미를 유지하면서 global identity, explicit create와 Framework runtime 경로에 맞춘다.',
        replacementCoverage: [],
        specOwner: 'framework/doc/framework/common/e2e/README.ko.md',
        runtimeOwner: languageOwner(scope.language, stage),
        activationStage: stage,
        quarantineStatus: 'pending-disabled-by-contract-amendment',
      };
      if (suite === 'SpotActorTransfer') {
        entries.push({
          ...suiteEntry,
          quarantineStatus: 'pending-disabled-reviewed-source',
          baselineHash: baselineHash(suitePath),
          approvedHash: currentTrackedHash(suitePath),
        });
      } else {
        addExecutionDisabledReviewedSourceEntry(suiteEntry);
      }
      for (const registration of registrationFiles(suitePath)) {
        const registrationEntry = {
          id: `registration:e2e:${scope.language}:${suite}:${sha256(registration.path).slice(0, 12)}`,
          kind: 'e2e-registration',
          language: scope.language,
          path: registration.path,
          disposition: 'retain',
          acceptanceIntent: 'source와 registration을 보존하고 승인된 activation stage 전에는 실행 graph에 넣지 않는다.',
          replacementCoverage: [],
          specOwner: 'framework/doc/framework/common/e2e/README.ko.md',
          runtimeOwner: languageOwner(scope.language, stage),
          activationStage: stage,
          quarantineStatus: 'pending-disabled-by-contract-amendment',
        };
        addReviewedRegistrationEntry(registrationEntry);
      }
    }
  }
  for (const sampleRoot of scope.samples) {
    if (baselineFiles(sampleRoot).length === 0) continue;
    for (const sample of directDirectories(sampleRoot)) {
      const samplePath = `${sampleRoot}/${sample}`;
      addExecutionDisabledReviewedSourceEntry({
        id: `sample:${scope.language}:${sample}`,
        kind: 'sample',
        language: scope.language,
        path: samplePath,
        disposition: 'amend',
        acceptanceIntent: '기존 사용자 흐름과 marker를 유지하면서 amended public contract만 사용한다.',
        replacementCoverage: [],
        specOwner: 'framework/doc/framework/common/sample/README.ko.md',
        runtimeOwner: languageOwner(scope.language, 'V11-M6B-E2E'),
        activationStage: 'V11-M7-SAMPLES',
        quarantineStatus: 'pending-disabled-by-contract-amendment',
      });
      for (const registration of registrationFiles(samplePath)) {
        addReviewedRegistrationEntry({
          id: `registration:sample:${scope.language}:${sample}:${sha256(registration.path).slice(0, 12)}`,
          kind: 'sample-registration',
          language: scope.language,
          path: registration.path,
          disposition: 'retain',
          acceptanceIntent: 'sample source와 runner registration을 보존하고 V11-M7-SAMPLES에서만 활성화한다.',
          replacementCoverage: [],
          specOwner: 'framework/doc/framework/common/sample/README.ko.md',
          runtimeOwner: languageOwner(scope.language, 'V11-M6B-E2E'),
          activationStage: 'V11-M7-SAMPLES',
          quarantineStatus: 'pending-disabled-by-contract-amendment',
        });
      }
    }
  }
}

for (const documentRoot of [
  {path: 'framework/doc/framework/common/e2e', kind: 'e2e-contract', activationStage: 'V11-E2E-SPEC-FINAL'},
  {path: 'framework/doc/framework/common/sample', kind: 'sample-contract', activationStage: 'V11-SAMPLE-SPEC-FINAL'},
]) {
  for (const record of baselineFiles(documentRoot.path).filter(item => item.path.endsWith('.ko.md'))) {
    const currentSpecOwner = record.path ===
      'framework/doc/framework/common/e2e/config-10-spot-actor-transfer.ko.md'
      ? 'framework/doc/framework/common/e2e/config-10-spot-actor-relocation.ko.md'
      : record.path;
    const contractEntry = {
      id: `${documentRoot.kind}:${path.basename(record.path, '.ko.md')}:${sha256(record.path).slice(0, 12)}`,
      kind: documentRoot.kind,
      language: 'common',
      path: currentSpecOwner,
      disposition: 'amend',
      acceptanceIntent: '통합된 formal contract와 runtime 검증 순서에 맞춰 scenario 또는 sample acceptance를 확정한다.',
      replacementCoverage: [],
      specOwner: currentSpecOwner,
      runtimeOwner: 'V11-M6-SCAFFOLD-ZERO',
      activationStage: documentRoot.activationStage,
      quarantineStatus: 'active-contract-spec',
    };
    if (currentSpecOwner === record.path) {
      addBaselineEntry(contractEntry);
    } else {
      entries.push({
        ...contractEntry,
        baselinePath: record.path,
        baselineHash: baselineHash(record.path),
        approvedHash: null,
      });
    }
  }
}

const regressionRoots = [
  ['cpp', 'framework/languages/cpp/tests'],
  ['dotnet', 'framework/languages/dotnet/tests'],
  ['java', 'framework/languages/java/zlink-framework-core/src/test'],
  ['java', 'framework/languages/java/zlink-framework-spring-boot-starter/src/test'],
  ['java', 'framework/languages/java/zlink-framework-locations-redis/src/test'],
  ['kotlin', 'framework/languages/java/zlink-framework-kotlin/src/test'],
  ['node', 'framework/languages/node/test'],
  ['common', 'framework/runtime/protocol'],
];
for (const [language, regressionPath] of regressionRoots) {
  if (baselineFiles(regressionPath).length === 0) continue;
  addBaselineEntry({
    id: `regression:${language}:${path.basename(regressionPath)}:${sha256(regressionPath).slice(0, 12)}`,
    kind: 'regression-root',
    language,
    path: regressionPath,
    disposition: 'retain',
    acceptanceIntent: 'runtime 구현 중 계속 실행하며 ordering, terminal winner, ownership, CAS, lease, fencing과 cleanup assertion을 약화하지 않는다.',
    replacementCoverage: [],
    specOwner: language === 'common'
      ? 'framework/doc/framework/common/internals/README.ko.md'
      : `framework/doc/framework/common/spec/server/languages/${language}/interfaces/README.ko.md`,
    runtimeOwner: language === 'common' ? 'V11-CA-PROTOCOL' : languageOwner(language, 'V11-M6A-E2E'),
    activationStage: 'V11-M6-SCAFFOLD-ZERO',
    quarantineStatus: 'active-regression',
  });
  for (const regression of regressionFiles(regressionPath, language)) {
    addReviewedRegressionEntry({
      id: `regression-test:${language}:${sha256(regression.path).slice(0, 16)}`,
      kind: 'regression-test',
      language,
      path: regression.path,
      disposition: 'retain',
      acceptanceIntent: '개별 deterministic regression의 assertion과 실행 registration을 유지하고 runtime 구현 중 계속 실행한다.',
      replacementCoverage: [],
      specOwner: language === 'common'
        ? 'framework/doc/framework/common/internals/README.ko.md'
        : `framework/doc/framework/common/spec/server/languages/${language}/interfaces/README.ko.md`,
      runtimeOwner: language === 'common'
        ? 'V11-CA-PROTOCOL'
        : languageOwner(language, 'V11-M6A-E2E'),
      activationStage: 'V11-M6-SCAFFOLD-ZERO',
      quarantineStatus: 'active-regression',
    });
  }
}

const rawRegressionRoots = [
  {scope: 'core', language: 'core', path: 'core/tests', runtimeOwner: 'V11-M3-CORE-VERIFY'},
  {scope: 'binding', language: 'cpp', path: 'bindings/cpp/tests', runtimeOwner: 'V11-M4-BIND-CPP'},
  {scope: 'binding', language: 'dotnet', path: 'bindings/dotnet/tests', runtimeOwner: 'V11-M4-BIND-DN'},
  {scope: 'binding', language: 'java', path: 'bindings/java/src/test', runtimeOwner: 'V11-M4-BIND-JVM'},
  {scope: 'binding', language: 'node', path: 'bindings/node/tests', runtimeOwner: 'V11-M4-BIND-NODE'},
];
for (const rawRoot of rawRegressionRoots) {
  const files = currentTrackedFiles(rawRoot.path);
  if (files.length === 0) throw new Error(`required raw regression root is empty: ${rawRoot.path}`);
  entries.push({
    id: `raw-regression-root:${rawRoot.scope}:${rawRoot.language}:${sha256(rawRoot.path).slice(0, 12)}`,
    kind: 'raw-regression-root',
    scope: rawRoot.scope,
    language: rawRoot.language,
    path: rawRoot.path,
    disposition: 'retain',
    baselineHash: currentTrackedHash(rawRoot.path),
    approvedHash: null,
    acceptanceIntent: '현재 tracked Core 또는 binding의 전체 raw regression source와 registration을 보존하고 Framework runtime 구현 중 계속 실행한다.',
    replacementCoverage: [],
    specOwner: 'framework/doc/plan/for-interals/framework-internals-implementation-gaps.ko.md',
    runtimeOwner: rawRoot.runtimeOwner,
    activationStage: 'V11-M6-SCAFFOLD-ZERO',
    quarantineStatus: 'active-regression',
  });
  for (const record of files) {
    entries.push({
      id: `raw-regression-test:${rawRoot.scope}:${rawRoot.language}:${sha256(record.path).slice(0, 16)}`,
      kind: 'raw-regression-test',
      scope: rawRoot.scope,
      language: rawRoot.language,
      path: record.path,
      disposition: 'retain',
      baselineHash: currentTrackedHash(record.path),
      approvedHash: null,
      acceptanceIntent: '현재 tracked raw regression file과 registration을 개별 항목으로 보존한다.',
      replacementCoverage: [],
      specOwner: 'framework/doc/plan/for-interals/framework-internals-implementation-gaps.ko.md',
      runtimeOwner: rawRoot.runtimeOwner,
      activationStage: 'V11-M6-SCAFFOLD-ZERO',
      quarantineStatus: 'active-regression',
    });
  }
}

const baselineTraceSource = revisionFile(baselineRevision, traceRelativePath);
const currentTraceSource = fs.readFileSync(tracePath, 'utf8');
const baselineTrace = JSON.parse(baselineTraceSource);
const currentTrace = JSON.parse(currentTraceSource);
for (const [label, trace] of [['baseline', baselineTrace], ['current', currentTrace]]) {
  if (!Array.isArray(trace.members) || trace.members.length === 0) {
    throw new Error(`${label} public-contract trace has no members`);
  }
}
const baselineTraceIdentities = new Set(baselineTrace.members.map(member => member.identity));
const currentTraceIdentities = new Set(currentTrace.members.map(member => member.identity));
const publicMemberAdds = currentTrace.members
  .filter(member => !baselineTraceIdentities.has(member.identity))
  .sort((left, right) => left.identity.localeCompare(right.identity, 'en'));
const publicMemberRemovals = baselineTrace.members
  .filter(member => !currentTraceIdentities.has(member.identity))
  .sort((left, right) => left.identity.localeCompare(right.identity, 'en'));
const publicMemberId = (disposition, member) =>
  `public-member:${disposition}:${member.language}:${sha256(member.identity).slice(0, 20)}`;
const additionsBySemanticMember = new Map();
for (const member of publicMemberAdds) {
  const key = semanticMemberKey(member);
  if (!additionsBySemanticMember.has(key)) additionsBySemanticMember.set(key, []);
  additionsBySemanticMember.get(key).push(member);
}
const invalidRemovalPolicies = publicMemberRemovals
  .filter(member => !(additionsBySemanticMember.get(semanticMemberKey(member))?.length))
  .map(member => ({member, audit: auditRemovedMemberBehavior(member)}))
  .filter(result => result.audit.state !== 'matched');
if (invalidRemovalPolicies.length > 0) {
  const counts = invalidRemovalPolicies.reduce((result, item) => {
    result[item.audit.state] = (result[item.audit.state] ?? 0) + 1;
    return result;
  }, {});
  const sample = invalidRemovalPolicies.slice(0, 30).map(item =>
    `${item.audit.state}[${item.audit.ruleIds.join(',')}]:${item.member.identity}`);
  throw new Error(`removed-member policy is not closed ${JSON.stringify(counts)}\n${sample.join('\n')}`);
}
const signatureReplacementCount = publicMemberRemovals.filter(member =>
  additionsBySemanticMember.get(semanticMemberKey(member))?.length).length;
const behaviorReplacementCount = publicMemberRemovals.length - signatureReplacementCount;
const behaviorByIdentity = new Map();
const parityGroups = new Map();
for (const member of publicMemberRemovals) {
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
  throw new Error('Kotlin source/JVM parity set changed without review'
    + ` groups=${sourceJvmParityGroups.length} sha256=${sourceJvmIdentitySetSha256}`);
}
const auditedParityGroups = [...new Map([
  ...crossLanguageParityGroups,
  ...sourceJvmParityGroups,
]).entries()];
const parityMismatches = auditedParityGroups.filter(([, group]) =>
  new Set(group.map(item => replacementParitySignature(item.behavior))).size > 1);
if (parityMismatches.length > 0) {
  throw new Error(`removed-member language parity mismatch: ${parityMismatches.map(([key]) => key).join(', ')}`);
}
const behaviorRuleCounts = Object.fromEntries([...behaviorByIdentity.values()]
  .reduce((counts, behavior) => {
    counts.set(behavior.ruleId, (counts.get(behavior.ruleId) ?? 0) + 1);
    return counts;
  }, new Map()).entries().toArray()
  .sort(([left], [right]) => left.localeCompare(right, 'en')));
for (const [ruleId, expected] of Object.entries(closedCatchAllExpectations)) {
  const identities = publicMemberRemovals.filter(member =>
    behaviorByIdentity.get(member.identity)?.ruleId === ruleId)
    .map(member => member.identity).sort((left, right) => left.localeCompare(right, 'en'));
  const identitySetSha256 = sha256(stableJson(identities));
  if (identities.length !== expected.count || identitySetSha256 !== expected.identitySetSha256) {
    throw new Error(`closed catch-all set changed without review: ${ruleId}`
      + ` count=${identities.length} sha256=${identitySetSha256}`);
  }
}
for (const member of publicMemberAdds) {
  const memberPath = member.exactInterface;
  entries.push({
    id: publicMemberId('add', member),
    kind: 'public-member',
    language: member.language,
    path: memberPath,
    disposition: 'add',
    baselineHash: null,
    approvedHash: null,
    memberIdentity: member.identity,
    memberName: member.memberName,
    ownerIdentity: member.ownerIdentity,
    acceptanceIntent: `${member.identity} public declaration과 contract test를 exact interface에 맞춘다.`,
    replacementCoverage: [],
    specOwner: memberPath,
    runtimeOwner: 'V11-M7-CONTRACT',
    activationStage: 'V11-M7-CONTRACT',
    quarantineStatus: 'pending-disabled-by-contract-amendment',
  });
}
for (const language of ['cpp', 'dotnet', 'java', 'kotlin', 'node']) {
  entries.push({
    id: `public-behavior:formal-contract-parity:${language}`,
    kind: 'public-behavior',
    language,
    path: null,
    disposition: 'add',
    baselineHash: null,
    approvedHash: null,
    acceptanceIntent: 'CA-D29에 따라 현재 source의 우연한 표면이 아니라 reviewed exact interface와 contract test로 공개 동작을 검증한다.',
    replacementCoverage: [],
    specOwner: `framework/doc/framework/common/spec/server/languages/${language}/interfaces/README.ko.md`,
    runtimeOwner: 'V11-M7-CONTRACT',
    activationStage: 'V11-M7-CONTRACT',
    quarantineStatus: 'pending-disabled-by-contract-amendment',
  });
}
for (const member of publicMemberRemovals) {
  const baselinePath = member.exactInterface;
  const replacements = additionsBySemanticMember.get(semanticMemberKey(member)) ?? [];
  const behavior = replacements.length === 0 ? behaviorByIdentity.get(member.identity) : null;
  entries.push({
    id: publicMemberId('remove', member),
    kind: 'public-member',
    language: member.language,
    path: null,
    baselinePath,
    disposition: 'remove',
    baselineHash: baselineHash(baselinePath),
    approvedHash: null,
    memberIdentity: member.identity,
    memberName: member.memberName,
    ownerIdentity: member.ownerIdentity,
    replacementKind: replacements.length > 0 ? 'signature' : 'behavior',
    replacementRule: replacements.length > 0 ? 'exact-semantic-member' : behavior.ruleId,
    decisionCoverage: behavior?.decisionCoverage ?? [],
    acceptanceIntent: replacements.length > 0
      ? `${member.identity} signature를 같은 semantic member의 exact trace identity로 교체한다.`
      : `${member.identity} 공개 member 제거를 ${behavior.decisionCoverage.join(', ')} 결정과 reviewed public behavior coverage로 검증한다.`,
    replacementCoverage: replacements.length > 0
      ? replacements.map(replacement => publicMemberId('add', replacement))
      : behavior.replacementCoverage,
    specOwner: replacements.length > 0
      ? replacements[0].exactInterface
      : 'framework/doc/framework/common/e2e/README.ko.md',
    runtimeOwner: 'V11-M7-CONTRACT',
    activationStage: 'V11-M7-CONTRACT',
    quarantineStatus: 'pending-disabled-by-contract-amendment',
  });
}

const plannedAdds = [
  ['e2e:add:global-actor-remote-create', 'e2e-scenario', 'global ActorId concurrent Create와 remote placement가 authority 하나로 수렴한다.', 'V11-M6B-E2E'],
  ['e2e:add:global-spot-explicit-create', 'e2e-scenario', 'global SpotId explicit Create와 stored intent reactivation을 검증한다.', 'V11-M6B-E2E'],
  ['e2e:add:exact-generation-mutation-bind', 'e2e-scenario', 'stale generation의 destroy, close와 session bind가 새 incarnation에 적용되지 않는다.', 'V11-M6B-E2E'],
  ['e2e:add:placement-capacity-weight', 'e2e-scenario', 'role, stable type capability, node-wide weight와 active·pending capacity 순서를 검증한다.', 'V11-M6A-E2E'],
  ['e2e:add:automatic-rid-collision', 'e2e-scenario', '128-bit random RID 충돌 8회 뒤 startup이 RoutingIdConflict로 끝난다.', 'V11-M6A-E2E'],
  ['e2e:add:reservation-crash-recovery', 'e2e-scenario', 'generic Reserve·Commit·Abort와 owner lease takeover가 pending capacity를 exact fence로 회수한다.', 'V11-M6C-E2E'],
  ['e2e:add:forwarding-bounds', 'e2e-scenario', 'forwarding 8 hops, 1024 messages와 16 MiB bound에서 typed terminal 결과를 검증한다.', 'V11-M6B-E2E'],
  ['sample:add:remote-object-create', 'sample-contract', 'Client와 Server role을 분리한 remote Actor·Spot create 사용 흐름을 제공한다.', 'V11-M7-SAMPLES'],
  ['regression:add:no-negative-route-cache', 'regression', 'Missing, Creating과 Store failure가 negative cache에 남지 않는지 검증한다.', 'V11-M6-SCAFFOLD-ZERO'],
  ['regression:add:global-authority-key', 'regression', 'MeshName과 독립적인 ActorId·SpotId authority key encoding을 네 runtime에서 검증한다.', 'V11-M6-SCAFFOLD-ZERO'],
  ['e2e:add:relocation-store-required-registration', 'e2e-scenario', 'Recreate 또는 Snapshot policy를 하나라도 등록한 Object Server가 Relocation Store를 정확히 하나 등록하고 socket bind 전에 검증을 통과한다.', 'V11-M6C-E2E'],
  ['e2e:add:relocation-store-registration-bind-failure', 'e2e-scenario', 'Recreate 또는 Snapshot policy에서 Relocation Store가 없거나 중복 등록되면 socket bind 전에 startup configuration error로 종료한다.', 'V11-M6C-E2E'],
  ['e2e:add:disabled-only-without-relocation-store', 'e2e-scenario', '모든 object policy가 Disabled인 Object Server는 Relocation Store 없이 시작하며 cross-node 이동을 Capture 전에 거부한다.', 'V11-M6C-E2E'],
  ['e2e:add:same-node-join-without-relocation-payload', 'e2e-scenario', 'same-node Actor join은 Relocation Store에 payload를 쓰거나 Location Store에 relocation reference를 publish하지 않는다.', 'V11-M6B-E2E'],
  ['e2e:add:redis-stores-shared-deployment', 'e2e-scenario', '공식 Redis Location Store와 Redis Relocation Store가 같은 Redis deployment를 사용하더라도 서로 다른 key prefix와 분리된 구현 class를 유지한다.', 'V11-M6C-E2E'],
  ['e2e:add:redis-stores-separate-deployments', 'e2e-scenario', 'Location Store와 Relocation Store를 물리적으로 다른 Redis deployment에 배치해도 cross-node 이동의 visibility와 recovery 의미가 동일하다.', 'V11-M6C-E2E'],
  ['e2e:add:published-relocation-data-lost', 'e2e-scenario', 'Location Store가 publish한 relocation reference의 payload가 영구적으로 없거나 checksum 또는 inventory digest가 맞지 않으면 RelocationDataLost로 종료하고 이전 owner로 rollback하지 않는다.', 'V11-M6C-E2E'],
  ['regression:add:relocation-write-before-location-cas', 'regression', 'immutable relocation root와 reference·checksum·retention 검증이 끝나기 전에 Location Store authority CAS가 실행되지 않는지 검증한다.', 'V11-M6-SCAFFOLD-ZERO'],
  ['regression:add:relocation-cas-conflict-orphan-cleanup', 'regression', 'Relocation Store 저장 뒤 Location CAS가 충돌하면 authority를 변경하지 않고 미공개 root를 orphan TTL 또는 idempotent delete 대상으로 남긴다.', 'V11-M6-SCAFFOLD-ZERO'],
  ['regression:add:relocation-root-replacement-order', 'regression', '새 immutable relocation root 저장, Location reference CAS, 이전 root 정리 순서를 고정하고 중간 실패가 published root를 손상하지 않는지 검증한다.', 'V11-M6-SCAFFOLD-ZERO'],
  ['regression:add:relocation-reference-release-before-delete', 'regression', 'Location Store가 relocation reference 사용 종료를 CAS한 뒤에만 Relocation Store payload를 삭제한다.', 'V11-M6-SCAFFOLD-ZERO'],
  ['regression:add:location-participant-digest-authority', 'regression', 'Location Store의 immutable inventory tree와 root·전체 count·digest가 authority다. Relocation manifest는 payload 탐색에만 사용하며 count와 digest가 모두 일치해야 한다.', 'V11-M6-SCAFFOLD-ZERO'],
  ['regression:add:relocation-data-lost-no-rollback', 'regression', 'published payload의 영구 누락, checksum 불일치와 participant inventory digest 불일치를 non-retriable RelocationDataLost로 분류하고 임의 rollback을 금지한다.', 'V11-M6-SCAFFOLD-ZERO'],
];
const plannedRuntimeLanguages = ['cpp', 'dotnet', 'java', 'node'];
const storeRegressionIds = plannedAdds
  .filter(([id, kind]) => kind === 'regression'
    && (id.includes('relocation-') || id.includes('location-participant-digest')))
  .flatMap(([id]) => plannedRuntimeLanguages.map(language => `${id}:${language}`));
for (const [id, kind, acceptanceIntent, activationStage] of plannedAdds) {
  const storeRelated = id.includes('relocation-store')
    || id.includes('relocation-')
    || id.includes('redis-stores')
    || id.includes('location-participant-digest');
  const specOwner = storeRelated
    ? 'framework/doc/framework/common/spec/23-relocation-store-redis.ko.md'
    : kind.startsWith('sample')
      ? 'framework/doc/framework/common/sample/README.ko.md'
      : 'framework/doc/framework/common/e2e/README.ko.md';
  if (kind === 'regression') {
    for (const language of plannedRuntimeLanguages) {
      entries.push({
        id: `${id}:${language}`,
        kind: 'regression',
        language,
        path: null,
        disposition: 'add',
        baselineHash: null,
        approvedHash: null,
        acceptanceIntent,
        replacementCoverage: [],
        specOwner,
        runtimeOwner: languageOwner(language, 'V11-M6A-E2E'),
        activationStage: 'V11-M6-SCAFFOLD-ZERO',
        quarantineStatus: 'planned-regression',
      });
    }
    continue;
  }
  entries.push({
    id,
    kind,
    language: 'common',
    path: null,
    disposition: 'add',
    baselineHash: null,
    approvedHash: null,
    acceptanceIntent,
    replacementCoverage: storeRelated && kind === 'e2e-scenario' ? storeRegressionIds : [],
    specOwner,
    runtimeOwner: kind.startsWith('sample') ? 'V11-M7-SAMPLES' : activationStage,
    activationStage,
    quarantineStatus: 'pending-disabled-by-contract-amendment',
  });
}

entries.sort((left, right) => left.id.localeCompare(right.id, 'en'));
const manifest = {
  schemaVersion: 1,
  version: '11.0.0',
  baselineRevision,
  contractDecisionRange: 'CA-D01..CA-D79',
  publicContractTraceDelta: {
    path: traceRelativePath,
    baselineSha256: sha256(baselineTraceSource),
    currentSha256: sha256(currentTraceSource),
    added: publicMemberAdds.length,
    removed: publicMemberRemovals.length,
    replacementPolicy: {
      signature: signatureReplacementCount,
      behavior: behaviorReplacementCount,
      unmatched: 0,
      ambiguous: 0,
      crossLanguageGroups: crossLanguageParityGroups.length,
      sourceJvmGroups: sourceJvmParityGroups.length,
      sourceJvmRecoveredPairs: sourceJvmParityExpectation.recoveredPairs,
      sourceJvmIdentitySetSha256,
      auditedParityGroups: auditedParityGroups.length,
      parityMismatches: 0,
      behaviorByRule: behaviorRuleCounts,
      closedCatchAll: closedCatchAllExpectations,
    },
  },
  state: 'pending-disabled-by-contract-amendment',
  execution: {executed: 0, skipped: 0},
  entries,
};

const command = process.argv[2];
if (!['--write', '--check'].includes(command)) {
  process.stderr.write('usage: generate-contract-amendment-impact.mjs --write|--check\n');
  process.exit(2);
}
const output = stableJson(manifest);
if (command === '--write') {
  fs.writeFileSync(manifestPath, output);
  process.stdout.write(`wrote ${path.relative(root, manifestPath)}: entries=${entries.length}\n`);
} else {
  if (!fs.existsSync(manifestPath) || fs.readFileSync(manifestPath, 'utf8') !== output) {
    throw new Error('contract amendment impact manifest is stale; run --write after reviewing baseline scope');
  }
  process.stdout.write(`verified ${path.relative(root, manifestPath)}: entries=${entries.length}\n`);
}
