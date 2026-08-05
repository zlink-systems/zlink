#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
inventory="$repo_root/framework/doc/contract-inventory/route-mesh-v11-document-contract-inventory.json"
wire_schema="$repo_root/framework/runtime/protocol/service-wire-v1.schema.json"
wire_validator="$repo_root/framework/runtime/protocol/validate-service-wire-schema.mjs"
public_contract_trace_generator="$repo_root/scripts/generate-v11-public-contract-trace.mjs"

node "$wire_validator" --self-test "$wire_schema"
node "$public_contract_trace_generator" --check

node - "$repo_root" "$inventory" <<'NODE'
'use strict';

const fs = require('fs');
const crypto = require('crypto');
const path = require('path');
const root = process.argv[2];
const inventoryPath = process.argv[3];
const {
  codeFenceFailures,
  duplicateCodeBlockFailures,
  duplicateDeclarationOwnerFailures,
  exactInterfaceBlockRole,
  fencedBlocks,
  headedFencedBlocks,
  publicDeclarationNames,
  readExactContract,
  relativeMarkdownLinkFailures,
} = require(path.join(root, 'scripts/lib/framework-contract-documents.cjs'));

const failures = [];
const fail = message => failures.push(message);
let javaKotlinSemanticNegativeTestCount = 0;
let hostLifecycleContractNegativeTestCount = 0;
if (!fs.existsSync(inventoryPath)) fail(`missing v11 document inventory: ${inventoryPath}`);
const inventory = fs.existsSync(inventoryPath)
  ? JSON.parse(fs.readFileSync(inventoryPath, 'utf8'))
  : {};
const traceConfigPath = path.join(
  root,
  'framework/doc/contract-inventory/route-mesh-v11-public-contract-trace.config.json');
if (!fs.existsSync(traceConfigPath)) fail(`missing v11 public-contract trace config: ${traceConfigPath}`);
const traceConfig = fs.existsSync(traceConfigPath)
  ? JSON.parse(fs.readFileSync(traceConfigPath, 'utf8'))
  : {};
const traceLanguages = new Map(
  (traceConfig.languages || []).map(language => [language.id, language]));

const expectedLanguages = ['dotnet', 'cpp', 'java', 'kotlin', 'node'];
const allowedDuplicateOwners = inventory.allowed_duplicate_declaration_owners || {};
if (inventory.schema !== 1 || inventory.version !== '11.0.0') {
  fail('v11 document inventory must use schema=1 and version=11.0.0');
}
if (JSON.stringify(inventory.languages) !== JSON.stringify(expectedLanguages)) {
  fail(`v11 document inventory language order differs: expected=${expectedLanguages.join(',')}`);
}
if (!inventory.exact_interfaces || typeof inventory.exact_interfaces !== 'object') {
  fail('v11 document inventory exact_interfaces object is missing');
}
for (const [language, owners] of Object.entries(allowedDuplicateOwners)) {
  if (!expectedLanguages.includes(language) || !owners || typeof owners !== 'object'
      || Array.isArray(owners)) {
    fail(`invalid allowed duplicate declaration-owner language: ${language}`);
    continue;
  }
  for (const [name, documents] of Object.entries(owners)) {
    if (!name || !Array.isArray(documents) || documents.length < 2
        || new Set(documents).size !== documents.length
        || documents.some(document => typeof document !== 'string'
          || !document.startsWith(
            `framework/doc/framework/common/spec/server/languages/${language}/interfaces/`))) {
      fail(`invalid allowed duplicate declaration owner: ${language}: ${name}`);
    }
  }
}
if (!Array.isArray(inventory.forbidden_public_code_patterns)
    || inventory.forbidden_public_code_patterns.length === 0) {
  fail('v11 document inventory forbidden_public_code_patterns must be a non-empty array');
}
if (!Array.isArray(inventory.formal_documents) || inventory.formal_documents.length === 0) {
  fail('v11 document inventory formal_documents must be a non-empty array');
}
for (const key of ['required_repository_file_fragments', 'forbidden_repository_file_fragments']) {
  if (!inventory[key] || typeof inventory[key] !== 'object' || Array.isArray(inventory[key])) {
    fail(`v11 document inventory ${key} must be an object`);
  }
}
if (!Array.isArray(inventory.consolidated_internal_document_sets)
    || inventory.consolidated_internal_document_sets.length !== 1) {
  fail('v11 document inventory must contain the common-internals document set');
} else if (inventory.consolidated_internal_document_sets[0].name !== 'common-internals') {
  fail('v11 consolidated internal document set must be common-internals');
}
if (!inventory.plan_consolidation || typeof inventory.plan_consolidation !== 'object') {
  fail('v11 document inventory plan_consolidation object is missing');
}
const expectedTerminationMembers = ['relocate', 'shutdown'];
const expectedTerminationOwners = ['ZLinkFrameworkRuntime'];
const expectedTopologyOwners = [
  'ZLinkRouteMeshRuntime',
  'ZLinkClientServerRuntime',
  'ZLinkFanoutRuntime',
];
const terminationOwnerPolicy = inventory.java_kotlin_host_lifecycle_owners || {};
if (JSON.stringify(terminationOwnerPolicy.members)
    !== JSON.stringify(expectedTerminationMembers)
    || JSON.stringify(terminationOwnerPolicy.allowed)
    !== JSON.stringify(expectedTerminationOwners)
    || JSON.stringify(terminationOwnerPolicy.forbidden_topologies)
    !== JSON.stringify(expectedTopologyOwners)) {
  fail('Java/Kotlin host lifecycle owner policy differs from the host-only contract');
}

const expectedHostLifecycleContract = {
  formal_path: 'framework/doc/framework/common/spec/28-graceful-drain-handoff.ko.md',
  e2e_path: 'framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md',
  states: [
    { name: 'Preparing', wire_value: 0 },
    { name: 'Serving', wire_value: 1 },
    { name: 'Relocating', wire_value: 2 },
    { name: 'Relocated', wire_value: 3 },
    { name: 'Draining', wire_value: 4 },
    { name: 'Stopped', wire_value: 5 },
    { name: 'Error', wire_value: 6 },
  ],
  relocation_outcomes: [
    { name: 'Relocated', wire_value: 0, reasons: ['None'] },
    { name: 'Blocked', wire_value: 1, reasons: [
      'TargetUnavailable', 'StoreUnavailable', 'RelocationDisabled', 'StateIncompatible',
      'DeadlineExceeded', 'RelocationFailed', 'RuntimeNotReady', 'ManualTopologyUnsupported',
      'ShutdownRequested', 'OperationInProgress',
    ] },
  ],
  relocation_reasons: [
    { name: 'None', wire_value: 0 },
    { name: 'TargetUnavailable', wire_value: 1 },
    { name: 'StoreUnavailable', wire_value: 2 },
    { name: 'RelocationDisabled', wire_value: 3 },
    { name: 'StateIncompatible', wire_value: 4 },
    { name: 'DeadlineExceeded', wire_value: 5 },
    { name: 'RelocationFailed', wire_value: 6 },
    { name: 'RuntimeNotReady', wire_value: 7 },
    { name: 'ManualTopologyUnsupported', wire_value: 8 },
    { name: 'ShutdownRequested', wire_value: 9 },
    { name: 'OperationInProgress', wire_value: 10 },
  ],
  termination_outcomes: [
    { name: 'Stopped', wire_value: 0, reasons: ['None'] },
    { name: 'ForceStopped', wire_value: 1, reasons: ['DeadlineExceeded', 'TeardownFailed'] },
  ],
  termination_reasons: [
    { name: 'None', wire_value: 0 },
    { name: 'DeadlineExceeded', wire_value: 1 },
    { name: 'TeardownFailed', wire_value: 2 },
  ],
  checkpoint_ceiling_pair: 'Blocked/StateIncompatible',
  forbidden_fragments: [
    'CheckpointTooLarge', 'Retire', 'Retiring', '| `Completed` |', '| `Failed` |',
  ],
};
const hostLifecycleContract = inventory.host_lifecycle_contract || {};
if (JSON.stringify(hostLifecycleContract)
    !== JSON.stringify(expectedHostLifecycleContract)) {
  fail('host lifecycle inventory differs from the closed formal contract');
}

const parseNamedValueTable = (source, headerPattern) => {
  const lines = source.split(/\r?\n/u);
  const header = lines.findIndex(line => headerPattern.test(line));
  if (header < 0) return [];
  const rows = [];
  for (let index = header + 2; index < lines.length && lines[index].startsWith('|'); index += 1) {
    const cells = lines[index].split('|').slice(1, -1).map(cell => cell.trim());
    if (cells.length < 2) continue;
    const name = /^`([^`]+)`$/u.exec(cells[1])?.[1];
    const wireValue = /^(0|[1-9][0-9]*)$/u.test(cells[0]) ? Number(cells[0]) : NaN;
    rows.push({ name, wire_value: wireValue });
  }
  return rows;
};

const parseRelocationOutcomeTable = source => {
  const lines = source.split(/\r?\n/u);
  const header = lines.findIndex(line => /^\|\s*값\s*\|\s*Outcome\s*\|\s*허용 reason\s*\|/u.test(line));
  if (header < 0) return [];
  const rows = [];
  for (let index = header + 2; index < lines.length && lines[index].startsWith('|'); index += 1) {
    const cells = lines[index].split('|').slice(1, -1).map(cell => cell.trim());
    if (cells.length < 3) continue;
    const name = /^`([^`]+)`$/u.exec(cells[1])?.[1];
    const wireValue = /^(0|[1-9][0-9]*)$/u.test(cells[0]) ? Number(cells[0]) : NaN;
    const reasons = [...cells[2].matchAll(/`([^`]+)`/gu)].map(match => match[1]);
    rows.push({ name, wire_value: wireValue, reasons });
  }
  return rows;
};

const parseRelocationReasonValues = source => {
  const start = source.indexOf('Reason은 `None=0`');
  if (start < 0) return [];
  const end = source.indexOf('이다.', start);
  if (end < 0) return [];
  return [...source.slice(start, end).matchAll(/`([A-Za-z][A-Za-z0-9]*)=(\d+)`/gu)]
    .map(match => ({ name: match[1], wire_value: Number(match[2]) }));
};

const parseShutdownValues = source => {
  const match = /Shutdown outcome은([\s\S]*?)다\./u.exec(source);
  if (!match) return { outcomes: [], reasons: [] };
  const reasonIndex = match[1].indexOf('reason은');
  if (reasonIndex < 0) return { outcomes: [], reasons: [] };
  const parse = value => [...value.matchAll(/`([A-Za-z][A-Za-z0-9]*)=(\d+)`/gu)]
    .map(entry => ({ name: entry[1], wire_value: Number(entry[2]) }));
  return {
    outcomes: parse(match[1].slice(0, reasonIndex)),
    reasons: parse(match[1].slice(reasonIndex)),
  };
};

const hostLifecycleDocumentFailures = (source, label) => {
  const messages = [];
  const states = parseNamedValueTable(
    source, /^\|\s*값\s*\|\s*State\s*\|\s*의미\s*\|/u);
  if (JSON.stringify(states) !== JSON.stringify(expectedHostLifecycleContract.states)) {
    messages.push(`${label} host lifecycle state wire values differ`);
  }
  if (JSON.stringify(parseRelocationOutcomeTable(source))
      !== JSON.stringify(expectedHostLifecycleContract.relocation_outcomes)) {
    messages.push(`${label} relocation outcome/reason pairs differ`);
  }
  if (JSON.stringify(parseRelocationReasonValues(source))
      !== JSON.stringify(expectedHostLifecycleContract.relocation_reasons)) {
    messages.push(`${label} relocation reason wire values differ`);
  }
  const shutdown = parseShutdownValues(source);
  const terminationOutcomes = shutdown.outcomes.map(outcome => ({
    ...outcome,
    reasons: outcome.name === 'Stopped' ? ['None'] : ['DeadlineExceeded', 'TeardownFailed'],
  }));
  if (JSON.stringify(terminationOutcomes)
      !== JSON.stringify(expectedHostLifecycleContract.termination_outcomes)) {
    messages.push(`${label} termination outcome/reason pairs differ`);
  }
  if (JSON.stringify(shutdown.reasons)
      !== JSON.stringify(expectedHostLifecycleContract.termination_reasons)) {
    messages.push(`${label} termination reason wire values differ`);
  }
  for (const fragment of expectedHostLifecycleContract.forbidden_fragments) {
    if (source.includes(fragment)) messages.push(`${label} contains forbidden ${fragment}`);
  }
  return messages;
};

const hostLifecycleSources = new Map();
for (const [label, relative] of [
  ['formal', expectedHostLifecycleContract.formal_path],
]) {
  const absolute = path.join(root, relative);
  if (!fs.existsSync(absolute)) {
    fail(`host lifecycle ${label} document is missing: ${relative}`);
    continue;
  }
  const source = fs.readFileSync(absolute, 'utf8');
  hostLifecycleSources.set(label, source);
  for (const message of hostLifecycleDocumentFailures(source, label)) fail(message);
}
const terminationE2ePath = path.join(root, expectedHostLifecycleContract.e2e_path);
if (!fs.existsSync(terminationE2ePath)) {
  fail(`host lifecycle E2E document is missing: ${expectedHostLifecycleContract.e2e_path}`);
} else {
  const source = fs.readFileSync(terminationE2ePath, 'utf8');
  if (!source.includes(`\`${expectedHostLifecycleContract.checkpoint_ceiling_pair}\``)) {
    fail(`host lifecycle E2E checkpoint ceiling must use ${expectedHostLifecycleContract.checkpoint_ceiling_pair}`);
  }
  for (const fragment of expectedHostLifecycleContract.forbidden_fragments) {
    if (source.includes(fragment)) fail(`host lifecycle E2E contains forbidden ${fragment}`);
  }
}

const hostLifecycleNegativeFixtures = [
  ['Relocating state removed', 'formal', source => source.replace('| 2 | `Relocating` |', '| 2 | `Serving` |')],
  ['Blocked deadline omitted', 'formal', source => source.replace(', `DeadlineExceeded`, `RelocationFailed`', ', `RelocationFailed`')],
  ['operation conflict reason omitted', 'formal', source => source.replace(', `OperationInProgress` |', ' |')],
  ['checkpoint reason widened', 'formal', source => source.replace('`StateIncompatible`', '`CheckpointTooLarge`')],
  ['termination outcome widened', 'formal', source => source.replace('`ForceStopped=1`', '`Failed=1`')],
  ['termination reason renumbered', 'formal', source => source.replace('`TeardownFailed=2`', '`TeardownFailed=3`')],
];
for (const [fixture, label, mutate] of hostLifecycleNegativeFixtures) {
  const source = hostLifecycleSources.get(label);
  if (!source) continue;
  const candidate = mutate(source);
  if (candidate === source
      || hostLifecycleDocumentFailures(candidate, `${label}:${fixture}`).length === 0) {
    fail(`host lifecycle contract negative self-test did not reject ${fixture}`);
  }
  hostLifecycleContractNegativeTestCount += 1;
}

const filesUnder = relativeDirectory => {
  const files = [];
  const visit = current => {
    const absolute = path.join(root, current);
    if (!fs.existsSync(absolute)) return;
    for (const entry of fs.readdirSync(absolute, { withFileTypes: true })) {
      const relative = path.posix.join(current, entry.name);
      if (entry.isDirectory()) visit(relative);
      else if (entry.isFile() || entry.isSymbolicLink()) files.push(relative);
    }
  };
  visit(relativeDirectory);
  return files.sort((left, right) => left.localeCompare(right, 'en'));
};
const markdownDocumentsUnder = relativeDirectory => filesUnder(relativeDirectory)
  .filter(relative => relative.endsWith('.ko.md'));
const maskCodeLiterals = source => {
  const masked = [...source];
  const blank = index => {
    if (masked[index] !== '\n' && masked[index] !== '\r') masked[index] = ' ';
  };
  for (let index = 0; index < source.length;) {
    if (source.startsWith('//', index)) {
      while (index < source.length && source[index] !== '\n') blank(index++);
      continue;
    }
    if (source.startsWith('/*', index)) {
      blank(index++);
      blank(index++);
      while (index < source.length && !source.startsWith('*/', index)) blank(index++);
      if (index < source.length) {
        blank(index++);
        blank(index++);
      }
      continue;
    }
    const quote = source[index];
    const tripleQuote = quote === '"' && source.startsWith('"""', index);
    if (tripleQuote) {
      for (let count = 0; count < 3; count += 1) blank(index++);
      while (index < source.length && !source.startsWith('"""', index)) blank(index++);
      for (let count = 0; count < 3 && index < source.length; count += 1) blank(index++);
      continue;
    }
    if (quote === '"' || quote === '\'' || quote === '`') {
      blank(index++);
      while (index < source.length) {
        if (source[index] === '\\') {
          blank(index++);
          if (index < source.length) blank(index++);
          continue;
        }
        const current = source[index];
        blank(index++);
        if (current === quote) break;
      }
      continue;
    }
    index += 1;
  }
  return masked.join('');
};

const matchingBrace = (masked, open) => {
  let depth = 0;
  for (let index = open; index < masked.length; index += 1) {
    if (masked[index] === '{') depth += 1;
    else if (masked[index] === '}') {
      depth -= 1;
      if (depth === 0) return index;
    }
  }
  return -1;
};

const declarationSpans = (source, tag) => {
  const masked = maskCodeLiterals(source);
  const pattern = tag === 'java'
    ? /\b(?:(?:public|protected|private|sealed|non-sealed|final|abstract|static)\s+)*(class|interface|record|enum|@interface)\s+([A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*)/gu
    : /\b(?:(?:public|protected|private|internal|sealed|data|enum|value|annotation|open|abstract|final|inner)\s+)*(class|interface|object)\s+([A-Za-z_$][\w$]*)/gu;
  const declarations = [];
  for (const match of masked.matchAll(pattern)) {
    const open = masked.indexOf('{', match.index + match[0].length);
    if (open < 0) continue;
    const close = matchingBrace(masked, open);
    if (close < 0) continue;
    const qualifiedName = match[2];
    declarations.push({
      kind: match[1],
      name: qualifiedName.slice(qualifiedName.lastIndexOf('.') + 1),
      nameEnd: match.index + match[0].length,
      start: match.index,
      open,
      close,
    });
  }
  return declarations;
};

const enclosingDeclaration = (declarations, index) => declarations
  .filter(declaration => declaration.start <= index && index < declaration.close)
  .sort((left, right) => (left.close - left.start) - (right.close - right.start))[0];

const terminationNameExpression = expectedTerminationMembers.join('|');
const terminationDeclarations = (source, tag) => {
  const masked = maskCodeLiterals(source);
  const declarations = declarationSpans(source, tag);
  const members = [];
  if (tag === 'java') {
    const pattern = new RegExp(
      `(?:^|[;{}])[ \\t]*(?!(?:return|throw|new)\\b)`
      + `(?:(?:@[A-Za-z_$][\\w$]*(?:\\([^\\n]*\\))?)[ \\t]+)*`
      + `(?:(?:public|protected|private|abstract|default|static|final|synchronized|native|strictfp)[ \\t]+)*`
      + `(?:[A-Za-z_$][^;={}\\n()]*?[ \\t]+)`
      + `(${terminationNameExpression})[ \\t]*\\(`,
      'gmu');
    for (const match of masked.matchAll(pattern)) {
      const memberIndex = match.index + match[0].lastIndexOf(match[1]);
      members.push({
        name: match[1],
        owner: enclosingDeclaration(declarations, memberIndex)?.name || '<Java top-level>',
        extension: false,
      });
    }
    return members;
  }

  const pattern = new RegExp(
    `^[ \\t]*(?:(?:public|protected|private|internal|override|open|final|abstract|suspend|operator|infix|inline|tailrec|external)[ \\t]+)*`
    + `fun[ \\t]+(?:<[^>\\n]+>[ \\t]*)?`
    + `(?:([A-Za-z_$][\\w$]*(?:\\.[A-Za-z_$][\\w$]*)*(?:<[^>\\n]+>)?\\??)[ \\t]*\\.[ \\t]*)?`
    + `(${terminationNameExpression})[ \\t]*\\(`,
    'gmu');
  for (const match of masked.matchAll(pattern)) {
    const memberIndex = match.index + match[0].lastIndexOf(match[2]);
    const receiver = match[1];
    members.push({
      name: match[2],
      owner: receiver
        ? `<Kotlin extension:${receiver}>`
        : enclosingDeclaration(declarations, memberIndex)?.name || '<Kotlin top-level>',
      extension: receiver !== undefined,
    });
  }
  return members;
};

const terminationOwnerFailures = (entries, language) => {
  const allowed = new Set(expectedTerminationOwners);
  const messages = [];
  for (const entry of entries) {
    for (const block of entry.blocks) {
      if (block.tag !== 'java' && block.tag !== 'kotlin') continue;
      for (const member of terminationDeclarations(block.source, block.tag)) {
        if (!allowed.has(member.owner)) {
          messages.push(
            `${language}: ${entry.relative}: ${member.owner}.${member.name} is not host-owned`);
        }
      }
    }
  }
  return messages;
};

const frameworkStatusFailures = source => {
  const masked = maskCodeLiterals(source);
  const records = declarationSpans(source, 'java')
    .filter(declaration => declaration.kind === 'record'
      && declaration.name === 'ZLinkFrameworkRuntimeStatus');
  const messages = [];
  if (records.length !== 1) {
    messages.push(`status record declaration count differs: actual=${records.length}`);
    return messages;
  }
  const record = records[0];
  const header = masked.slice(record.nameEnd, record.open);
  const body = masked.slice(record.open + 1, record.close);
  const requiredComponents = [
    [/\bZLinkFrameworkRuntimeState\s+state\b/gu, 'state'],
    [/\bboolean\s+isReady\b/gu, 'isReady'],
    [/\bboolean\s+acceptingWork\b/gu, 'acceptingWork'],
    [/\bOptional\s*<\s*ZLinkFrameworkRelocationResult\s*>\s+relocationResult\b/gu,
      'relocationResult'],
    [/\bOptional\s*<\s*ZLinkFrameworkTerminationResult\s*>\s+terminationResult\b/gu,
      'terminationResult'],
    [/\blong\s+sequence\b/gu, 'sequence'],
    [/\bInstant\s+observedAt\b/gu, 'observedAt'],
  ];
  for (const [pattern, name] of requiredComponents) {
    const typedCount = [...header.matchAll(pattern)].length;
    const nameCount = [...header.matchAll(new RegExp(`\\b${name}\\b`, 'gu'))].length;
    if (typedCount !== 1 || nameCount !== 1) {
      messages.push(
        `status ${name} component count differs: typed=${typedCount} named=${nameCount}`);
    }
  }
  const explicitAccessorCount = [
    ...body.matchAll(
      /\b(?:state|isReady|acceptingWork|relocationResult|terminationResult|sequence|observedAt)\s*\(/gu),
  ].length;
  if (explicitAccessorCount !== 0) {
    messages.push(`status repeats generated accessor: actual=${explicitAccessorCount}`);
  }
  return messages;
};

let exactDocumentCount = 0;
let exactFenceCount = 0;
let syntaxAndExampleFenceCount = 0;
let packageDeclarationOwnerCount = 0;
let blockRoleNegativeMutationCount = 0;
const exactFenceRoleCounts = new Map([
  ['package-contract', 0],
  ['application-example', 0],
  ['documentation-support', 0],
]);
for (const language of expectedLanguages) {
  const projection = inventory.exact_interfaces?.[language];
  if (!projection) {
    fail(`missing exact-interface projection: ${language}`);
    continue;
  }
  for (const key of ['code_tags', 'required_fragments']) {
    if (!Array.isArray(projection[key]) || projection[key].length === 0) {
      fail(`${language} ${key} must be a non-empty array`);
    }
  }
  for (const key of ['required_file_fragments', 'forbidden_file_fragments']) {
    if (projection[key] !== undefined
        && (!projection[key] || typeof projection[key] !== 'object'
            || Array.isArray(projection[key]))) {
      fail(`${language} ${key} must be an object when present`);
    }
  }
  for (const key of ['minimum_documents', 'minimum_code_blocks']) {
    if (!Number.isInteger(projection[key]) || projection[key] <= 0) {
      fail(`${language} ${key} must be a positive integer`);
    }
  }

  const languageRoleConfig = traceLanguages.get(language);
  if (!languageRoleConfig) {
    fail(`${language} public-contract trace language policy is missing`);
    continue;
  }
  if (JSON.stringify(languageRoleConfig.acceptedCodeTags)
      !== JSON.stringify(projection.code_tags)) {
    fail(`${language} DOC and TRACE syntax tag policies differ`);
  }
  const contract = readExactContract(root, language);
  contract.entries = contract.entries.map(entry => ({
    ...entry,
    blocks: entry.blocks.map(block => ({
      ...block,
      ...exactInterfaceBlockRole(traceConfig, languageRoleConfig, entry.relative, block),
    })),
  }));
  contract.code = contract.entries.flatMap(entry => entry.blocks
    .filter(block => block.role === 'package-contract')
    .map(block => block.source)).join('\n');
  exactDocumentCount += contract.documents.length;
  const blocks = contract.entries.flatMap(entry => entry.blocks);
  const syntaxAndExampleBlocks = blocks
    .filter(block => block.role === 'package-contract' || block.role === 'application-example');
  exactFenceCount += blocks.length;
  syntaxAndExampleFenceCount += syntaxAndExampleBlocks.length;
  for (const block of blocks) {
    if (!exactFenceRoleCounts.has(block.role)) {
      fail(`${language} exact fence has an unknown role: ${block.role}`);
      continue;
    }
    exactFenceRoleCounts.set(block.role, exactFenceRoleCounts.get(block.role) + 1);
  }
  if (contract.documents.length < projection.minimum_documents) {
    fail(`${language} exact document count is too small: expected>=${projection.minimum_documents} actual=${contract.documents.length}`);
  }
  if (!contract.documents.some(relative => relative.endsWith('/README.ko.md'))) {
    fail(`${language} exact interface README is missing`);
  }
  if (syntaxAndExampleBlocks.length < projection.minimum_code_blocks) {
    fail(`${language} syntax/example fence count is too small: expected>=${projection.minimum_code_blocks} actual=${syntaxAndExampleBlocks.length}`);
  }
  for (const fragment of projection.required_fragments || []) {
    if (!contract.source.includes(fragment)) {
      fail(`${language} exact contract is missing semantic projection: ${fragment}`);
    }
  }
  const exactDirectory = path.posix.join(
    'framework/doc/framework/common/spec/server/languages', language, 'interfaces');
  for (const [name, fragments] of Object.entries(projection.required_file_fragments || {})) {
    if (!Array.isArray(fragments) || fragments.length === 0) {
      fail(`${language} required_file_fragments must contain non-empty arrays: ${name}`);
      continue;
    }
    const relative = path.posix.join(exactDirectory, name);
    const absolute = path.join(root, relative);
    if (!fs.existsSync(absolute)) {
      fail(`${language} exact semantic owner is missing: ${relative}`);
      continue;
    }
    const source = fs.readFileSync(absolute, 'utf8');
    for (const fragment of fragments) {
      if (!source.includes(fragment)) {
        fail(`${language} exact semantic owner is missing ${fragment}: ${relative}`);
      }
    }
  }
  for (const [name, fragments] of Object.entries(projection.forbidden_file_fragments || {})) {
    if (!Array.isArray(fragments) || fragments.length === 0) {
      fail(`${language} forbidden_file_fragments must contain non-empty arrays: ${name}`);
      continue;
    }
    const relative = path.posix.join(exactDirectory, name);
    const absolute = path.join(root, relative);
    if (!fs.existsSync(absolute)) {
      fail(`${language} exact semantic owner is missing: ${relative}`);
      continue;
    }
    const source = fs.readFileSync(absolute, 'utf8');
    for (const fragment of fragments) {
      if (source.includes(fragment)) {
        fail(`${language} exact semantic owner contains forbidden duplicate ${fragment}: ${relative}`);
      }
    }
  }
  for (const expression of inventory.forbidden_public_code_patterns || []) {
    let pattern;
    try {
      pattern = new RegExp(expression, 'u');
    } catch (error) {
      fail(`invalid forbidden public-code pattern ${expression}: ${error.message}`);
      continue;
    }
    if (pattern.test(contract.code)) {
      fail(`${language} exact public declaration contains forbidden v11 surface: ${expression}`);
    }
  }
  for (const message of codeFenceFailures(root, contract.documents)) {
    fail(`exact code fence: ${message}`);
  }
  for (const message of relativeMarkdownLinkFailures(root, contract.documents)) {
    fail(`exact link: ${message}`);
  }
  const syntaxAndExampleContract = {
    ...contract,
    entries: contract.entries.map(entry => ({
      ...entry,
      blocks: entry.blocks.filter(block => block.role !== 'documentation-support'),
    })),
  };
  for (const message of duplicateCodeBlockFailures(language, syntaxAndExampleContract)) {
    fail(`duplicate exact code block: ${message}`);
  }
  const duplicateOwners = duplicateDeclarationOwnerFailures(language, contract);
  const observedAllowedDuplicateOwners = new Set();
  for (const message of duplicateOwners) {
    const prefix = `${language}: `;
    const separator = message.indexOf(': ', prefix.length);
    const name = separator < 0 ? '' : message.slice(prefix.length, separator);
    const documents = separator < 0
      ? []
      : message.slice(separator + 2).split(', ').sort();
    const allowedDocuments = allowedDuplicateOwners[language]?.[name];
    if (!allowedDocuments
        || JSON.stringify(documents) !== JSON.stringify([...allowedDocuments].sort())) {
      fail(`duplicate exact declaration owner: ${message}`);
      continue;
    }
    observedAllowedDuplicateOwners.add(name);
  }
  for (const name of Object.keys(allowedDuplicateOwners[language] || {})) {
    if (!observedAllowedDuplicateOwners.has(name)) {
      fail(`allowed duplicate declaration owner is stale or incomplete: ${language}: ${name}`);
    }
  }
  const ownerNames = new Set();
  for (const entry of contract.entries) {
    const code = entry.blocks
      .filter(block => block.role === 'package-contract')
      .map(block => block.source)
      .join('\n');
    for (const name of publicDeclarationNames(language, code)) ownerNames.add(name);
  }
  if (ownerNames.size === 0) fail(`${language} exact public declaration inventory is empty`);
  packageDeclarationOwnerCount += ownerNames.size;
}

const applicationExampleMutationSource = [
  '## Example fixture',
  '',
  '```csharp',
  'public sealed class MustNotBecomePackageOwner {}',
  '```',
].join('\n');
const applicationExampleMutationDocument =
  'framework/doc/framework/common/spec/server/languages/dotnet/interfaces/99-examples.ko.md';
const dotnetRoleConfig = traceLanguages.get('dotnet');
const applicationExampleMutationBlocks = headedFencedBlocks(applicationExampleMutationSource)
  .map(block => ({
    ...block,
    ...exactInterfaceBlockRole(
      traceConfig,
      dotnetRoleConfig,
      applicationExampleMutationDocument,
      block),
  }));
const applicationExampleMutationOwnerNames = new Set(applicationExampleMutationBlocks
  .filter(block => block.role === 'package-contract')
  .flatMap(block => [...publicDeclarationNames('dotnet', block.source)]));
if (applicationExampleMutationBlocks.length !== 1
    || applicationExampleMutationBlocks[0].role !== 'application-example'
    || applicationExampleMutationOwnerNames.size !== 0) {
  fail('application example declaration entered the package declaration-owner inventory');
} else {
  blockRoleNegativeMutationCount += 1;
}

const javaExactDirectory =
  'framework/doc/framework/common/spec/server/languages/java/interfaces';
const javaMonitoringRelative = path.posix.join(javaExactDirectory, 'monitoring.ko.md');
const javaMonitoringSource = fs.readFileSync(path.join(root, javaMonitoringRelative), 'utf8');
const javaSourceHeadings = [
  '2. Host 상태',
  '3. RouteMesh 상태',
];
const javaSourceBlocks = new Map();
for (const heading of javaSourceHeadings) {
  const sourceBlocks = headedFencedBlocks(javaMonitoringSource)
    .filter(block => block.tag === 'java' && block.heading === heading);
  if (sourceBlocks.length !== 1) {
    fail(`Java exact source heading must own one java block: ${heading}`);
    continue;
  }
  const block = sourceBlocks[0];
  javaSourceBlocks.set(heading, block);
  const memberSource = block.source.split(/\r?\n/u)
    .filter(line => !/^\s*(?:package|import)\s+/u.test(line))
    .join('\n');
  const fullyQualifiedType =
    /\b(?:java|javax|jakarta|org|com|io|systems|kotlin|kotlinx)\.[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)+/u;
  if (fullyQualifiedType.test(memberSource)) {
    fail(`Java exact source block repeats a fully-qualified type: ${javaMonitoringRelative}:${block.startLine}`);
  }
}
const javaHostObservation = javaSourceBlocks.get(
  '2. Host 상태');
if (javaHostObservation) {
  for (const message of frameworkStatusFailures(javaHostObservation.source)) {
    fail(`Java host runtime status contract: ${javaMonitoringRelative}:${javaHostObservation.startLine}: ${message}`);
  }
}

const javaCommonRelative = path.posix.join(javaExactDirectory, 'common-runtime.ko.md');
const javaCommonSource = fs.readFileSync(path.join(root, javaCommonRelative), 'utf8');
const javaCommonCode = fencedBlocks(javaCommonSource, ['java'])
  .map(block => block.source).join('\n');
const javaRuntimeOwnerPattern =
  /\bpublic\s+final\s+class\s+(?:systems\.zlink\.framework\.runtime\.host\.)?ZLinkFrameworkRuntime\b/gu;
const javaRuntimeOwnerCount = source => [...source.matchAll(javaRuntimeOwnerPattern)].length;
if (javaRuntimeOwnerCount(javaCommonCode) !== 1) {
  fail(`Java runtime must have one canonical exact declaration owner: actual=${javaRuntimeOwnerCount(javaCommonCode)}`);
}
for (const accessor of ['routeMeshRuntime', 'clientServerRuntime', 'fanoutRuntime']) {
  const pattern = new RegExp(`\\b${accessor}\\s*\\(\\s*\\)\\s*;`, 'gu');
  const count = [...javaCommonCode.matchAll(pattern)].length;
  if (count !== 1) {
    fail(`Java runtime topology accessor must have one canonical declaration: ${accessor} actual=${count}`);
  }
}
const javaCommonNormalized = javaCommonCode.replace(/\s+/gu, ' ');
for (const fragment of [
  'implements AutoCloseable, ZLinkMessageFlowControl',
  'public ZLinkFrameworkRuntimeStatus status();',
  'public Flow.Publisher<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> observe();',
  'public CompletionStage<ZLinkFrameworkRelocationResult> relocate(',
  'public CompletionStage<ZLinkFrameworkTerminationResult> shutdown();',
]) {
  if (!javaCommonNormalized.includes(fragment)) {
    fail(`Java canonical runtime declaration is missing: ${fragment}`);
  }
}
if (javaRuntimeOwnerCount(`${javaCommonCode}\npublic final class systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime {}`) !== 2) {
  fail('Java duplicate runtime owner negative self-test fixture is invalid');
} else {
  javaKotlinSemanticNegativeTestCount += 1;
}
for (const [label, pattern] of [
  ['public runtime constructor', /\bpublic\s+ZLinkFrameworkRuntime\s*\(/u],
  ['public runtime factory', /\bpublic\s+static\s+(?:systems\.zlink\.framework\.runtime\.host\.)?ZLinkFrameworkRuntime\s+\w+\s*\(/u],
  ['public runtime bootstrap member', /\bpublic\s+[^;{}\n]+\s+(?:bootstrap|start)\s*\(/u],
]) {
  if (pattern.test(javaCommonCode)) {
    fail(`Java runtime exact surface contains forbidden ${label}`);
  }
}

const javaConfigurationRelative = path.posix.join(
  javaExactDirectory, 'configuration-host.ko.md');
const javaConfigurationSource = fs.readFileSync(
  path.join(root, javaConfigurationRelative), 'utf8');
const javaConfigurationCode = fencedBlocks(javaConfigurationSource, ['java'])
  .map(block => block.source).join('\n');
for (const [label, pattern] of [
  ['runtime.internal type', /systems\.zlink\.framework\.runtime\.internal/u],
  ['auto-configuration implementation', /ZLinkFrameworkAutoConfiguration/u],
  ['framework lifecycle implementation', /ZLinkFrameworkLifecycle/u],
  ['monitoring lifecycle implementation', /ZLinkMonitoringLifecycle/u],
  ['public runtime bean factory', /\bzlinkFrameworkRuntime\s*\(/u],
]) {
  if (pattern.test(javaConfigurationCode)) {
    fail(`Java configuration public contract exposes ${label}`);
  }
}

const javaLifecycleRelative =
  'framework/doc/framework/java/internals/runtime-lifecycle.ko.md';
const javaLifecycleSource = fs.readFileSync(path.join(root, javaLifecycleRelative), 'utf8');
if (!/^\| `ZLinkFrameworkAutoConfigurationTest\.exposesSingleRuntimeAndTopologyRuntimeBeans` \| [^\n]*`assertSame`[^\n]*\|$/mu.test(javaLifecycleSource)) {
  fail('Java singleton topology bean test must compare each facade view by reference identity');
}

const kotlinSourceContract = readExactContract(root, 'kotlin', ['kotlin']);
const kotlinAsFlowDeclarations = [];
const kotlinAsFlowPattern =
  /\b(?:public\s+)?fun\s*(?:<[^>]+>\s*)?[^(){};=]*?\.asFlow\s*\(\s*\)/gu;
for (const entry of kotlinSourceContract.entries) {
  for (const block of entry.blocks) {
    const normalized = block.source.replace(/\s+/gu, ' ').trim();
    for (const ignored of normalized.matchAll(kotlinAsFlowPattern)) {
      kotlinAsFlowDeclarations.push(entry.relative);
    }
  }
}
if (kotlinAsFlowDeclarations.length !== 0) {
  fail(`Kotlin exact contract must not add an asFlow source wrapper: actual=${kotlinAsFlowDeclarations.join(',')}`);
}
const kotlinGeneratedContract = readExactContract(root, 'kotlin', ['java']);
const kotlinGeneratedAsFlowOwners = [];
for (const entry of kotlinGeneratedContract.entries) {
  for (const block of entry.blocks) {
    for (const ignored of block.source.matchAll(/\basFlow\s*\(/gu)) {
      kotlinGeneratedAsFlowOwners.push(entry.relative);
    }
  }
}
if (kotlinGeneratedAsFlowOwners.length !== 0) {
  fail(`Kotlin exact contract must not add a generated asFlow wrapper: actual=${kotlinGeneratedAsFlowOwners.join(',')}`);
}

const javaTerminationContract = readExactContract(root, 'java', ['java']);
const kotlinTerminationContract = readExactContract(root, 'kotlin', ['kotlin', 'java']);
for (const [language, contract] of [
  ['java', javaTerminationContract],
  ['kotlin', kotlinTerminationContract],
]) {
  for (const message of terminationOwnerFailures(contract.entries, language)) {
    fail(`host-only termination owner: ${message}`);
  }
  const terminationPattern = /\b(?:relocate|shutdown)\s*\(([^)]*)\)/gu;
  for (const match of contract.code.matchAll(terminationPattern)) {
    const parameters = match[1];
    if (/\b(?:java\.lang\.)?String\b|\b[Mm]eshName\b|\btarget\b/u.test(parameters)) {
      fail(`${language} host termination member contains a topology target parameter: ${match[0]}`);
    }
  }
}

const cppTerminationContract = readExactContract(root, 'cpp', ['cpp']);
const cppTerminationNormalized = cppTerminationContract.source.replace(/\s+/gu, ' ');
if (/\b(?:drain_force_reason_t|drain_result_t|drained_t|force_stopped_t)\b/u
  .test(cppTerminationContract.code)
  || /\b(?:drain|await_drained)\s*\(/u.test(cppTerminationContract.code)) {
  fail('C++ exact contract retains a removed deprecated drain facade');
}
if (!cppTerminationNormalized.includes(
  '기존 `retire()`, `drain()`과 `await_drained()`는 공개 interface에서 제거한다.')) {
  fail('C++ exact contract is missing the deprecated lifecycle removal statement');
}

const terminationOwnerNegativeFixtures = [
  {
    label: 'Java RouteMesh parameterless shutdown',
    tag: 'java',
    source: 'public interface ZLinkRouteMeshRuntime { void shutdown(); }',
  },
  {
    label: 'Kotlin topology extension shutdown',
    tag: 'kotlin',
    source: 'fun ZLinkRouteMeshRuntime.shutdown(): Unit = Unit',
  },
  {
    label: 'generated Kotlin extension relocate',
    tag: 'java',
    source: 'public final class ZLinkRuntimeExtensionsKt { public static java.lang.Object relocate(); }',
  },
];
for (const fixture of terminationOwnerNegativeFixtures) {
  const messages = terminationOwnerFailures([{
    relative: `<negative:${fixture.label}>`,
    blocks: [{ tag: fixture.tag, source: fixture.source }],
  }], 'self-test');
  if (messages.length !== 1) {
    fail(`termination owner negative self-test did not reject ${fixture.label}`);
  } else {
    javaKotlinSemanticNegativeTestCount += 1;
  }
}
for (const fixture of [
  'public final class ZLinkFrameworkRuntime { public void shutdown(); }',
  'public final class ZLinkFrameworkRuntime { public void relocate(); }',
]) {
  const messages = terminationOwnerFailures([{
    relative: '<allowed:host termination>',
    blocks: [{ tag: 'java', source: fixture }],
  }], 'self-test');
  if (messages.length !== 0) {
    fail(`termination owner allowed self-test rejected a host owner: ${messages.join(', ')}`);
  }
}

const validStatusFixture = `
public record ZLinkFrameworkRuntimeStatus(
    ZLinkFrameworkRuntimeState state,
    boolean isReady,
    boolean acceptingWork,
    Optional<Instant> deadline,
    Optional<ZLinkFrameworkRelocationResult> relocationResult,
    Optional<ZLinkFrameworkTerminationResult> terminationResult,
    long sequence,
    Instant observedAt) {}`;
const statusNegativeFixtures = [
  validStatusFixture.replace(
    '    Optional<ZLinkFrameworkRelocationResult> relocationResult,\n', ''),
  validStatusFixture.replace(
    '    Optional<ZLinkFrameworkTerminationResult> terminationResult,',
    '    Optional<ZLinkFrameworkTerminationResult> terminationResult,\n'
      + '    Optional<ZLinkFrameworkTerminationResult> terminationResult,'),
  validStatusFixture.replace('{}',
    '{ public boolean isReady() { return isReady; } }'),
];
if (frameworkStatusFailures(validStatusFixture).length !== 0) {
  fail('Java host status positive self-test rejected the required record components');
}
for (const [index, fixture] of statusNegativeFixtures.entries()) {
  if (frameworkStatusFailures(fixture).length === 0) {
    fail(`Java host status negative self-test did not reject mutation ${index + 1}`);
  } else {
    javaKotlinSemanticNegativeTestCount += 1;
  }
}

for (const [relative, fragments] of Object.entries(
  inventory.required_repository_file_fragments || {})) {
  if (!Array.isArray(fragments) || fragments.length === 0) {
    fail(`required_repository_file_fragments must contain non-empty arrays: ${relative}`);
    continue;
  }
  const absolute = path.join(root, relative);
  if (!fs.existsSync(absolute)) {
    fail(`required repository document is missing: ${relative}`);
    continue;
  }
  const source = fs.readFileSync(absolute, 'utf8');
  for (const fragment of fragments) {
    if (!source.includes(fragment)) {
      fail(`repository document is missing ${fragment}: ${relative}`);
    }
  }
}
for (const [relative, fragments] of Object.entries(
  inventory.forbidden_repository_file_fragments || {})) {
  if (!Array.isArray(fragments) || fragments.length === 0) {
    fail(`forbidden_repository_file_fragments must contain non-empty arrays: ${relative}`);
    continue;
  }
  const absolute = path.join(root, relative);
  if (!fs.existsSync(absolute)) {
    fail(`required repository document is missing: ${relative}`);
    continue;
  }
  const source = fs.readFileSync(absolute, 'utf8');
  for (const fragment of fragments) {
    if (source.includes(fragment)) {
      fail(`repository document contains forbidden fragment ${fragment}: ${relative}`);
    }
  }
}

const formalPaths = [];
for (const fixture of inventory.formal_documents || []) {
  if (!fixture || typeof fixture.path !== 'string'
      || !Array.isArray(fixture.required_fragments)
      || !Array.isArray(fixture.forbidden_fragments)) {
    fail('formal document fixture has an invalid schema');
    continue;
  }
  const absolute = path.join(root, fixture.path);
  if (!fs.existsSync(absolute)) {
    fail(`missing formal document: ${fixture.path}`);
    continue;
  }
  formalPaths.push(fixture.path);
  const source = fs.readFileSync(absolute, 'utf8');
  for (const fragment of fixture.required_fragments) {
    if (!source.includes(fragment)) fail(`formal document is missing ${fragment}: ${fixture.path}`);
  }
  for (const fragment of fixture.forbidden_fragments) {
    if (source.includes(fragment)) fail(`formal document contains removed contract ${fragment}: ${fixture.path}`);
  }
}
// Every formal Korean spec participates in structural validation. The
// inventory above selects semantic owners; it intentionally does not freeze
// prose or code-block hashes.
const allFormalPaths = markdownDocumentsUnder('framework/doc/framework/common/spec');
if (allFormalPaths.length === 0) fail('formal framework spec document set is empty');
for (const message of codeFenceFailures(root, allFormalPaths)) fail(`formal code fence: ${message}`);
for (const message of relativeMarkdownLinkFailures(root, allFormalPaths)) fail(`formal link: ${message}`);

let targetDocumentCount = 0;
for (const set of inventory.consolidated_internal_document_sets || []) {
  if (!set || typeof set.name !== 'string' || typeof set.directory !== 'string'
      || !Array.isArray(set.required_documents)
      || !Array.isArray(set.readme_required_fragments)
      || !Array.isArray(set.aggregate_required_fragments)
      || !Array.isArray(set.forbidden_code_patterns)
      || !set.required_file_fragments
      || typeof set.required_file_fragments !== 'object') {
    fail('consolidated internal document set has an invalid schema');
    continue;
  }
  const documents = markdownDocumentsUnder(set.directory);
  targetDocumentCount += documents.length;
  const actualNames = documents.map(relative => path.posix.basename(relative));
  for (const required of set.required_documents) {
    if (!actualNames.includes(required)) fail(`${set.name} is missing required document: ${required}`);
  }
  const readmeRelative = path.posix.join(set.directory, 'README.ko.md');
  const readmeAbsolute = path.join(root, readmeRelative);
  if (!fs.existsSync(readmeAbsolute)) {
    fail(`${set.name} README is missing`);
    continue;
  }
  const readme = fs.readFileSync(readmeAbsolute, 'utf8');
  const sources = documents.map(relative => fs.readFileSync(path.join(root, relative), 'utf8'));
  const aggregate = sources.join('\n');
  for (const fragment of set.readme_required_fragments) {
    if (!readme.includes(fragment)) fail(`${set.name} README is missing no-loss semantic: ${fragment}`);
  }
  for (const fragment of set.aggregate_required_fragments) {
    if (!aggregate.includes(fragment)) fail(`${set.name} is missing target semantic: ${fragment}`);
  }
  for (const [name, fragments] of Object.entries(set.required_file_fragments)) {
    if (!Array.isArray(fragments)) {
      fail(`${set.name} required_file_fragments must contain arrays: ${name}`);
      continue;
    }
    const relative = path.posix.join(set.directory, name);
    const absolute = path.join(root, relative);
    if (!fs.existsSync(absolute)) {
      fail(`${set.name} semantic owner is missing: ${name}`);
      continue;
    }
    const source = fs.readFileSync(absolute, 'utf8');
    for (const fragment of fragments) {
      if (!source.includes(fragment)) fail(`${set.name}/${name} is missing semantic: ${fragment}`);
    }
  }
  const code = sources.flatMap(source => fencedBlocks(source).map(block => block.source)).join('\n');
  for (const expression of set.forbidden_code_patterns) {
    let pattern;
    try {
      pattern = new RegExp(expression, 'u');
    } catch (error) {
      fail(`invalid ${set.name} forbidden-code pattern ${expression}: ${error.message}`);
      continue;
    }
    if (pattern.test(code)) fail(`${set.name} contains removed Core service C ABI code: ${expression}`);
  }
  for (const message of codeFenceFailures(root, documents)) fail(`${set.name} code fence: ${message}`);
  for (const message of relativeMarkdownLinkFailures(root, documents)) fail(`${set.name} link: ${message}`);
}

let forbiddenLegacyPlanCount = 0;
let planDocumentCount = 0;
let consolidatedSemanticCount = 0;

const consolidation = inventory.plan_consolidation || {};
if (typeof consolidation.directory !== 'string'
    || !Array.isArray(consolidation.allowed_documents)
    || !Array.isArray(consolidation.temporary_review_documents)
    || !Array.isArray(consolidation.forbidden_fragments)
    || !Array.isArray(consolidation.required_semantic_owners)
    || !Array.isArray(consolidation.forbidden_legacy_documents)) {
  fail('v11 plan_consolidation has an invalid current-plan schema');
} else {
  const safeRelativePath = relative => typeof relative === 'string'
    && relative.length > 0
    && !path.posix.isAbsolute(relative)
    && !relative.split('/').includes('..');
  const allowedDocuments = consolidation.allowed_documents;
  const temporaryDocuments = consolidation.temporary_review_documents;
  const stableSet = new Set(allowedDocuments);
  const temporarySet = new Set(temporaryDocuments);
  const allowedSet = new Set([...allowedDocuments, ...temporaryDocuments]);
  if (allowedDocuments.length === 0
      || stableSet.size !== allowedDocuments.length
      || allowedDocuments.some(relative => !safeRelativePath(relative))) {
    fail('v11 current plan must declare unique safe relative document paths');
  }
  if (temporarySet.size !== temporaryDocuments.length
      || temporaryDocuments.some(relative => !safeRelativePath(relative)
        || stableSet.has(relative))) {
    fail('v11 current plan temporary documents must be unique safe paths outside the stable set');
  }

  const actualDocuments = filesUnder(consolidation.directory)
    .map(relative => path.posix.relative(consolidation.directory, relative));
  planDocumentCount = actualDocuments.length;
  const actualSet = new Set(actualDocuments);
  for (const relativeName of allowedSet) {
    if (!actualSet.has(relativeName)) {
      fail(`v11 current plan document is missing: ${relativeName}`);
    }
  }
  for (const relativeName of actualDocuments) {
    if (!allowedSet.has(relativeName)) {
      fail(`v11 current plan contains an unowned document: ${relativeName}`);
    }
  }

  const semanticIds = new Set();
  if (consolidation.required_semantic_owners.length === 0) {
    fail('v11 current plan must declare semantic owners');
  }
  for (const owner of consolidation.required_semantic_owners) {
    if (!owner || typeof owner.id !== 'string' || owner.id.length === 0
        || semanticIds.has(owner.id)
        || typeof owner.path !== 'string' || !allowedSet.has(owner.path)
        || !Array.isArray(owner.required_fragments)
        || owner.required_fragments.length === 0
        || owner.required_fragments.some(fragment => typeof fragment !== 'string'
          || fragment.length === 0)) {
      fail('v11 current plan semantic owner has an invalid schema');
      continue;
    }
    semanticIds.add(owner.id);
    const relative = path.posix.join(consolidation.directory, owner.path);
    const absolute = path.join(root, relative);
    if (!fs.existsSync(absolute)) {
      fail(`v11 current plan semantic owner is missing: ${owner.id}: ${owner.path}`);
      continue;
    }
    consolidatedSemanticCount += 1;
    const source = fs.readFileSync(absolute, 'utf8');
    for (const fragment of owner.required_fragments) {
      if (!source.includes(fragment)) {
        fail(`v11 current plan semantic is missing: ${owner.id}: ${fragment}`);
      }
    }
  }

  const planSources = actualDocuments.map(relative =>
    fs.readFileSync(path.join(root, consolidation.directory, relative), 'utf8'));
  for (const fragment of consolidation.forbidden_fragments) {
    if (typeof fragment !== 'string' || fragment.length === 0) {
      fail('v11 current plan forbidden_fragments must contain non-empty strings');
      continue;
    }
    if (planSources.some(source => source.includes(fragment))) {
      fail(`v11 current plan contains a forbidden policy fragment: ${fragment}`);
    }
  }
  const planPaths = actualDocuments.map(relative =>
    path.posix.join(consolidation.directory, relative));
  for (const message of codeFenceFailures(root, planPaths)) {
    fail(`v11 current plan code fence: ${message}`);
  }
  for (const message of relativeMarkdownLinkFailures(root, planPaths)) {
    fail(`v11 current plan link: ${message}`);
  }

  for (const relativeName of consolidation.forbidden_legacy_documents) {
    if (typeof relativeName !== 'string' || relativeName.length === 0) {
      fail('v11 forbidden legacy document paths must be non-empty strings');
      continue;
    }
    forbiddenLegacyPlanCount += 1;
    const absolute = path.join(root, relativeName);
    if (fs.existsSync(absolute)) {
      fail(`legacy v11 plan document must not return: ${relativeName}`);
    }
  }
}

// Redis fixtures are semantic wire examples. Validate their schema, field
// order and length-prefixed keys without turning ordinary prose edits into a
// hash failure.
const redisFixtures = [
  ['actor-location-v2.json', 'actor-location-v2'],
  ['authority-store-v3.json', 'location-authority-hybrid-v3'],
  ['client-server-server-descriptor-v1.json', 'client-server-server-descriptor-v1'],
  ['fanout-publisher-descriptor-v1.json', 'fanout-publisher-descriptor-v1'],
  ['mesh-node-descriptor-v1.json', 'mesh-node-descriptor-v1'],
];
const fixtureDirectory = path.join(root, 'framework/testdata/location/redis');
for (const obsolete of ['actor-relocation-v1.json', 'instance-spot-location-v1.json']) {
  if (fs.existsSync(path.join(fixtureDirectory, obsolete))) {
    fail(`obsolete phase-specific Redis fixture remains: ${obsolete}`);
  }
}
const descriptorKey = (...values) => values
  .map(value => `${Buffer.byteLength(value, 'utf8')}:${value}`).join('');
const serviceWireSchema = JSON.parse(fs.readFileSync(path.join(
  root, 'framework/runtime/protocol/service-wire-v1.schema.json'), 'utf8'));
const authorityKeyProfile = serviceWireSchema.authorityKeyFormat;
const unreservedAuthorityByte = byte => (byte >= 0x41 && byte <= 0x5a)
  || (byte >= 0x61 && byte <= 0x7a)
  || (byte >= 0x30 && byte <= 0x39)
  || [0x2d, 0x2e, 0x5f, 0x7e].includes(byte);
const encodeAuthorityComponent = bytes => `${bytes.length}:` + [...bytes]
  .map(byte => unreservedAuthorityByte(byte)
    ? String.fromCharCode(byte)
    : `%${byte.toString(16).toUpperCase().padStart(2, '0')}`)
  .join('');
const canonicalAuthorityKey = keyContract => {
  const kind = authorityKeyProfile.kindDiscriminators.find(
    candidate => candidate.objectKind === keyContract?.objectKind);
  const identityHex = keyContract?.identityHex || '';
  if (!kind || !/^(?:[0-9a-fA-F]{2})+$/u.test(identityHex)) return undefined;
  const identity = Buffer.from(identityHex, 'hex');
  if (identity.length < 1 || identity.length > 255
      || !Buffer.from(identity.toString('utf8'), 'utf8').equals(identity)) return undefined;
  return [authorityKeyProfile.prefix, kind.wire,
    encodeAuthorityComponent(identity)]
    .join(authorityKeyProfile.separator);
};
const validateHashFields = (fixtureName, fixture, hash) => {
  if (!Array.isArray(fixture.hashFields)
      || new Set(fixture.hashFields).size !== fixture.hashFields.length
      || !hash || typeof hash !== 'object'
      || JSON.stringify(Object.keys(hash)) !== JSON.stringify(fixture.hashFields)) {
    fail(`Redis fixture hash field schema differs: ${fixtureName}`);
  }
};
const positiveDecimal = value => typeof value === 'string'
  && /^[1-9]\d*$/u.test(value)
  && BigInt(value) <= 9223372036854775807n;
const validateAuthorityHash = (fixtureName, fixture, hash) => {
  const fields = fixture.currentHashFields ?? fixture.hashFields;
  if (!Array.isArray(fields) || new Set(fields).size !== fields.length
      || !hash || typeof hash !== 'object'
      || JSON.stringify(Object.keys(hash)) !== JSON.stringify(fields)) {
    fail(`Redis authority fixture hash field schema differs: ${fixtureName}`);
  }
  if (typeof hash?.payload !== 'string' || hash.payload.length === 0
      || !positiveDecimal(hash.storeVersion)
      || !positiveDecimal(hash.objectGeneration)
      || !positiveDecimal(hash.authorityOwnerGeneration)
      || typeof hash.ownerId !== 'string' || hash.ownerId.length === 0
      || !positiveDecimal(hash.ownerLeaseGeneration)
      || Object.hasOwn(hash, 'leaseExpiresAtMs')) {
    fail(`Redis authority fixture metadata differs: ${fixtureName}`);
  }
  if (fixture.currentHashFields
      && (hash.authorityKey !== fixture.keyContract?.authorityKey
        || !['pending', 'active'].includes(hash.allocationState)
        || !['actor', 'user_spot', 'instance_spot'].includes(hash.objectKind)
        || typeof hash.stableType !== 'string' || hash.stableType.length === 0
        || typeof hash.descriptorKey !== 'string' || hash.descriptorKey.length === 0
        || !positiveDecimal(hash.descriptorLifecycleGeneration)
        || !positiveDecimal(hash.capacityDelta))) {
    fail(`Redis authority fixture allocation metadata differs: ${fixtureName}`);
  }
};
const validateAuthorityV2Hash = (fixtureName, fixture, hash, fields, state) => {
  if (!Array.isArray(fields) || new Set(fields).size !== fields.length
      || !hash || typeof hash !== 'object'
      || JSON.stringify(Object.keys(hash)) !== JSON.stringify(fields)) {
    fail(`Redis authority v2 hash field schema differs: ${fixtureName}`);
    return;
  }
  const expectedCapacityBundle = hash?.objectKind === 'actor'
    ? fixture.contextCapacityBundles?.actor
    : fixture.contextCapacityBundles?.userSpot;
  if (hash.authorityKey !== fixture.keyContract?.authorityKey
      || typeof hash.payload !== 'string' || hash.payload.length === 0
      || !positiveDecimal(hash.storeVersion)
      || !positiveDecimal(hash.objectGeneration)
      || !positiveDecimal(hash.authorityOwnerGeneration)
      || typeof hash.ownerId !== 'string' || hash.ownerId.length === 0
      || !positiveDecimal(hash.ownerLeaseGeneration)
      || hash.allocationState !== state
      || !['actor', 'user_spot', 'instance_spot'].includes(hash.objectKind)
      || typeof hash.stableType !== 'string' || hash.stableType.length === 0
      || typeof hash.descriptorKey !== 'string' || hash.descriptorKey.length === 0
      || !positiveDecimal(hash.descriptorLifecycleGeneration)
      || hash.capacityBundle !== expectedCapacityBundle
      || Object.hasOwn(hash, 'capacityDelta')
      || Object.hasOwn(hash, 'leaseExpiresAtMs')) {
    fail(`Redis authority v2 metadata differs: ${fixtureName}`);
  }
};
const exactFieldMap = (fixtureName, fields, value) => {
  if (!Array.isArray(fields) || new Set(fields).size !== fields.length
      || !value || typeof value !== 'object'
      || JSON.stringify(Object.keys(value)) !== JSON.stringify(fields)) {
    fail(`Redis exact field map differs: ${fixtureName}`);
    return false;
  }
  return true;
};
const validateHashRow = (fixtureName, row, expectedKey = undefined) => {
  if (!row || typeof row !== 'object' || !row.hash || typeof row.hash !== 'object') {
    fail(`Redis fixture row/hash is missing: ${fixtureName}`);
    return undefined;
  }
  if (expectedKey !== undefined && row.key !== expectedKey) {
    fail(`Redis fixture length-prefixed key differs: ${fixtureName}`);
  }
  let payload;
  try {
    payload = JSON.parse(row.hash.json);
  } catch (error) {
    fail(`Redis fixture embedded JSON is invalid: ${fixtureName}: ${error.message}`);
    return undefined;
  }
  if (row.hash.json !== JSON.stringify(payload)) {
    fail(`Redis fixture embedded JSON is not canonical compact JSON: ${fixtureName}`);
  }
  return payload;
};
const descriptorFieldOrder = {
  'client-server-server-descriptor-v1.json': [
    'ChannelName', 'ServerRid', 'LifecycleGeneration', 'DescriptorRevision',
    'Endpoint', 'Weight', 'State', 'SecurityIdentity', 'OwnerId',
    'OwnerLeaseGeneration', 'UpdatedAt',
  ],
  'fanout-publisher-descriptor-v1.json': [
    'ChannelName', 'PublisherRid', 'LifecycleGeneration', 'DescriptorRevision',
    'Endpoint', 'State', 'SecurityIdentity', 'OwnerId', 'OwnerLeaseGeneration',
    'UpdatedAt',
  ],
  'mesh-node-descriptor-v1.json': [
    'MeshName', 'Rid', 'LifecycleGeneration', 'DescriptorRevision',
    'Endpoint', 'ChannelWeights', 'SecurityIdentity', 'OwnerId',
    'LeaseGeneration', 'UpdatedAt', 'ApplicationVersion',
    'ObjectCapabilities', 'MaintenanceWave', 'State', 'ObjectRole',
    'EntrySpotId', 'PlacementWeight', 'Capacity', 'ActivationConcurrency',
  ],
};
const runtimeStates = new Set(['Preparing', 'Serving', 'Draining', 'Stopped', 'Error']);
const compareUtf8 = (left, right) => Buffer.compare(
  Buffer.from(left, 'utf8'), Buffer.from(right, 'utf8'));
const isStrictlyUtf8Sorted = values => Array.isArray(values)
  && values.every((value, index) => typeof value === 'string'
    && (index === 0 || compareUtf8(values[index - 1], value) < 0));
const validateDescriptorPayload = (name, payload) => {
  const expectedFields = descriptorFieldOrder[name];
  if (!expectedFields) return;
  if (JSON.stringify(Object.keys(payload)) !== JSON.stringify(expectedFields)) {
    fail(`Redis descriptor canonical field order differs: ${name}`);
  }
  const meshNode = name === 'mesh-node-descriptor-v1.json';
  if (meshNode
      ? !Number.isInteger(payload.State) || payload.State < 0
      : !runtimeStates.has(payload.State)) {
    fail(`Redis descriptor FrameworkRuntimeState differs: ${name}`);
  }
  const generationFields = meshNode
    ? ['LifecycleGeneration', 'DescriptorRevision', 'LeaseGeneration']
    : ['LifecycleGeneration', 'DescriptorRevision', 'OwnerLeaseGeneration'];
  for (const field of generationFields) {
    if (!Number.isSafeInteger(payload[field]) || payload[field] <= 0) {
      fail(`Redis descriptor ${field} must be a positive safe integer: ${name}`);
    }
  }
  if (typeof payload.OwnerId !== 'string' || payload.OwnerId.length === 0) {
    fail(`Redis descriptor OwnerId is missing: ${name}`);
  }
  if (name !== 'mesh-node-descriptor-v1.json') return;
  const channelNames = Object.keys(payload.ChannelWeights || {});
  if (!isStrictlyUtf8Sorted(channelNames)) {
    fail('MeshNode descriptor ChannelWeights must be UTF-8 sorted and unique');
  }
  if (!Number.isSafeInteger(payload.ApplicationVersion) || payload.ApplicationVersion < 0) {
    fail('MeshNode descriptor ApplicationVersion must be a non-negative JSON integer');
  }
  if (!(payload.MaintenanceWave === null || typeof payload.MaintenanceWave === 'string')) {
    fail('MeshNode descriptor MaintenanceWave must be a string or null');
  }
  if (!Array.isArray(payload.ObjectCapabilities)
      || payload.ObjectCapabilities.length > 1024) {
    fail('MeshNode descriptor ObjectCapabilities count differs');
    return;
  }
  if (!Number.isInteger(payload.ObjectRole)
      || !Number.isInteger(payload.PlacementWeight)
      || payload.PlacementWeight < 0 || payload.PlacementWeight > 10000
      || !payload.Capacity) {
    fail('MeshNode descriptor placement metadata differs');
  }
  const validUsage = usage => usage
    && JSON.stringify(Object.keys(usage)) === JSON.stringify([
      'Active', 'Reserved', 'Limit'])
    && ['Active', 'Reserved', 'Limit'].every(field =>
      Number.isInteger(usage[field]) && usage[field] >= 0)
    && (usage.Limit === 0 || usage.Active + usage.Reserved <= usage.Limit);
  if (!validUsage(payload.Capacity?.Actors)
      || !validUsage(payload.Capacity?.Spots)
      || !Array.isArray(payload.Capacity?.SpotTypes)) {
    fail('MeshNode descriptor typed capacity differs');
  }
  let previousSpotType;
  for (const spotType of payload.Capacity?.SpotTypes || []) {
    if (JSON.stringify(Object.keys(spotType)) !== JSON.stringify([
      'ObjectKind', 'StableType', 'Usage'])
        || ![2, 3].includes(spotType.ObjectKind)
        || typeof spotType.StableType !== 'string'
        || spotType.StableType.length === 0
        || !validUsage(spotType.Usage)) {
      fail('MeshNode descriptor SpotTypes capacity differs');
      continue;
    }
    const canonical = `${spotType.ObjectKind}\0${spotType.StableType}`;
    if (previousSpotType !== undefined
        && compareUtf8(previousSpotType, canonical) >= 0) {
      fail('MeshNode descriptor SpotTypes order differs');
    }
    previousSpotType = canonical;
  }
  const kindOrder = { 1: 1, 2: 2, 3: 3 };
  let previousCapability;
  for (const capability of payload.ObjectCapabilities) {
    if (JSON.stringify(Object.keys(capability)) !== JSON.stringify([
      'ObjectKind', 'StableType', 'Policy', 'HasSnapshotAdapter', 'SpotLimit'])) {
      fail('MeshNode descriptor ObjectCapabilities field order differs');
      continue;
    }
    if (kindOrder[capability.ObjectKind] === undefined
        || typeof capability.StableType !== 'string' || capability.StableType.length === 0
        || ![1, 2, 3].includes(capability.Policy)
        || typeof capability.HasSnapshotAdapter !== 'boolean'
        || !Number.isSafeInteger(capability.SpotLimit)
        || capability.SpotLimit < 0) {
      fail('MeshNode descriptor ObjectCapabilities value differs');
    }
    if ((capability.Policy === 3) !== capability.HasSnapshotAdapter) {
      fail('MeshNode descriptor relocation policy and snapshot adapter capability differ');
    }
    const current = `${capability.ObjectKind}\u0000${capability.StableType}`;
    if (previousCapability !== undefined) {
      const [previousKind, previousType] = previousCapability.split('\u0000');
      if (kindOrder[previousKind] > kindOrder[capability.ObjectKind]
          || (previousKind === capability.ObjectKind
            && compareUtf8(previousType, capability.StableType) >= 0)) {
        fail('MeshNode descriptor ObjectCapabilities must be sorted and unique');
      }
    }
    previousCapability = current;
  }
};
let redisFixtureCount = 0;
for (const [name, format] of redisFixtures) {
  const absolute = path.join(fixtureDirectory, name);
  if (!fs.existsSync(absolute)) {
    fail(`missing Redis semantic fixture: ${name}`);
    continue;
  }
  let fixture;
  try {
    fixture = JSON.parse(fs.readFileSync(absolute, 'utf8'));
  } catch (error) {
    fail(`invalid Redis semantic fixture JSON: ${name}: ${error.message}`);
    continue;
  }
  redisFixtureCount += 1;
  if (fixture.format !== format) fail(`Redis fixture format differs: ${name}`);
  if (name === 'authority-store-v3.json') {
    const expectedKey = canonicalAuthorityKey(fixture.keyContract);
    const authorityBytes = Buffer.from(expectedKey || '', 'utf8');
    const authorityHex = authorityBytes.toString('hex');
    const authorityDigest = crypto.createHash('sha256')
      .update(authorityBytes).digest('hex');
    const physicalBase = 'P:{zlink-location-v3}';
    if (!expectedKey || fixture.keyContract?.authorityKey !== expectedKey
        || fixture.prefixRules?.literalHashTag !== '{zlink-location-v3}'
        || fixture.prefixRules?.schemaKey !== `${physicalBase}:schema`
        || fixture.prefixRules?.schemaFields?.format
          !== 'location-authority-hybrid-v3'
        || fixture.prefixRules?.schemaFields?.epoch !== '3'
        || fixture.keyContract?.authorityKeyHex !== authorityHex
        || fixture.keyContract?.authorityKeySha256 !== authorityDigest
        || fixture.keyContract?.currentKey
          !== `${physicalBase}:authority:current:${authorityDigest}`
        || fixture.keyContract?.historyKey
          !== `${physicalBase}:authority:history:${authorityDigest}`
        || fixture.keyContract?.historyRevisionKey
          !== `${physicalBase}:authority:history-revisions:${authorityDigest}`
        || fixture.keyContract?.indexKey
          !== `${physicalBase}:authority:key-index`) {
      fail('Authority Store v3 physical key schema differs');
    }

    const expectedCurrentFields = [
      'authorityKey', 'payload', 'storeVersion', 'objectGeneration',
      'authorityOwnerGeneration', 'ownerId', 'ownerLeaseGeneration',
      'allocationState', 'objectKind', 'stableType', 'descriptorKey',
      'descriptorLifecycleGeneration', 'capacityBundle',
    ];
    const expectedReservedFields = [
      ...expectedCurrentFields,
      'pendingCreationReservationId', 'pendingCreationReference',
      'pendingCreationSha256', 'pendingCreationEncodedSize',
    ];
    if (JSON.stringify(fixture.currentHashFields)
          !== JSON.stringify(expectedCurrentFields)
        || JSON.stringify(fixture.reservedCurrentHashFields)
          !== JSON.stringify(expectedReservedFields)) {
      fail('Authority Store v2 current HASH field schema differs');
    }

    const segment = value =>
      `${Buffer.byteLength(String(value), 'utf8')}:${String(value)}`;
    const bundle = fixture.capacityBundle || {};
    const encodedBundle = [
      bundle.domain,
      bundle.actorSlots,
      bundle.spotSlots,
      bundle.spotTypePresence,
      bundle.spotTypeObjectKind,
      bundle.spotTypeStableType,
      bundle.spotTypeSlots,
    ].map(segment).join('');
    if (bundle.domain !== 'zlink-capacity-bundle-v2'
        || bundle.spotTypePresence !== '1'
        || !['user_spot', 'instance_spot'].includes(
          bundle.spotTypeObjectKind)
        || bundle.encoded !== encodedBundle
        || bundle.hex !== Buffer.from(encodedBundle, 'utf8').toString('hex')) {
      fail('Authority Store v2 capacityBundle encoding differs');
    }
    const contextBundles = fixture.contextCapacityBundles || {};
    const actorBundle = [
      'zlink-capacity-bundle-v2', '1', '0', '0',
    ].map(segment).join('');
    const userSpotBundle = [
      'zlink-capacity-bundle-v2', '0', '1', '1',
      'user_spot', 'room', '1',
    ].map(segment).join('');
    if (contextBundles.actor !== actorBundle
        || contextBundles.userSpot !== userSpotBundle
        || contextBundles.aggregate !== encodedBundle) {
      fail('Authority Store v2 contextual capacity bundles differ');
    }

    const capacity = fixture.capacityBuckets || {};
    const expectedNodeBucket =
      segment(capacity.descriptorKey)
      + segment(capacity.descriptorLifecycleGeneration)
      + segment('spot');
    const expectedSpotTypeBucket =
      expectedNodeBucket
      + segment(capacity.objectKind)
      + segment(capacity.stableType);
    const expectedUnicodeSpotTypeBucket =
      expectedNodeBucket
      + segment(capacity.objectKind)
      + segment(capacity.unicodeStableType);
    if (capacity.segmentLengthUnit !== 'UTF-8 bytes'
        || capacity.node !== expectedNodeBucket
        || capacity.spotType !== expectedSpotTypeBucket
        || capacity.unicodeSpotType !== expectedUnicodeSpotTypeBucket
        || !['user_spot', 'instance_spot'].includes(capacity.objectKind)) {
      fail('Authority Store v2 capacity bucket encoding differs');
    }

    validateAuthorityV2Hash(
      name, fixture, fixture.reserve?.currentHash,
      fixture.reservedCurrentHashFields, 'reserved');
    for (const transition of ['commit', 'preserve', 'newOwner']) {
      validateAuthorityV2Hash(
        name, fixture, fixture[transition]?.currentHash,
        fixture.currentHashFields, 'active');
    }
    if (!/^[0-9a-f]{32}$/u.test(
          fixture.reserve?.currentHash?.pendingCreationReservationId || '')
        || !/^[0-9a-f]{64}$/u.test(
          fixture.reserve?.currentHash?.pendingCreationSha256 || '')
        || fixture.reserve?.currentHash?.pendingCreationEncodedSize !== '0') {
      fail('Authority Store v2 Reserved creation projection differs');
    }

    const expectedRecordFields = {
      creation: [
        'state', 'reservationId', 'authorityKey', 'storeVersion',
        'objectGeneration', 'authorityOwnerGeneration', 'reservationVersion',
        'objectKind', 'stableType', 'targetDescriptorKey',
        'targetDescriptorLifecycleGeneration', 'targetOwnerId',
        'targetOwnerLeaseGeneration', 'creationReference', 'creationSha256',
        'creationEncodedSize', 'capacityBundle',
      ],
      creationTerminal: [
        'state', 'sourceNodeRid', 'sourceNodeGeneration', 'operationIdHigh',
        'operationIdLow', 'reservationId', 'objectKind', 'terminalEnvelope',
        'terminalEnvelopeSha256', 'expiresAtUnixMs',
      ],
      standaloneRelocation: [
        'state', 'reservationId', 'authorityKey', 'expectedStoreVersion',
        'objectKind', 'stableType', 'sourceDescriptorKey',
        'sourceDescriptorLifecycleGeneration', 'sourceOwnerId',
        'sourceOwnerLeaseGeneration', 'targetDescriptorKey',
        'targetDescriptorLifecycleGeneration', 'targetOwnerId',
        'targetOwnerLeaseGeneration', 'capacityBundle',
      ],
      aggregate: [
        'state', 'aggregateId', 'aggregateGeneration', 'participants',
        'inventoryDigest', 'targetDescriptorKey',
        'targetDescriptorLifecycleGeneration', 'targetOwnerId',
        'targetOwnerLeaseGeneration', 'capacityBundle',
      ],
    };
    const recordIds = {
      creation: '00112233445566778899aabbccddeeff',
      creationTerminal: '00112233445566778899aabbccddeeff',
      standaloneRelocation: '11112222333344445555666677778888',
      aggregate: '22223333444455556666777788889999',
    };
    const recordStates = {
      creation: 'Reserved',
      creationTerminal: 'Rejected',
      standaloneRelocation: 'Committed',
      aggregate: 'Aborted',
    };
    const recordKeys = {
      creation: `${physicalBase}:creation:${recordIds.creation}`,
      creationTerminal:
        `${physicalBase}:creation-terminal:6:6e6f64652d61:7:00000000000000000000000000000001`,
      standaloneRelocation:
        `${physicalBase}:relocation:${recordIds.standaloneRelocation}`,
      aggregate: `${physicalBase}:aggregate:${recordIds.aggregate}:9`,
    };
    const recordCapacityBundles = {
      creation: contextBundles.userSpot,
      standaloneRelocation: contextBundles.actor,
      aggregate: contextBundles.aggregate,
    };
    for (const recordName of Object.keys(expectedRecordFields)) {
      const fields = fixture.operationRecordFieldSets?.[recordName];
      const record = fixture.operationRecords?.[recordName];
      const hash = record?.hash;
      if (JSON.stringify(fields)
            !== JSON.stringify(expectedRecordFields[recordName])
          || !exactFieldMap(
            `authority-store-v3/${recordName}`, fields, hash)
          || record?.key !== recordKeys[recordName]
          || hash?.state !== recordStates[recordName]
          || (hash?.reservationId ?? hash?.aggregateId)
            !== recordIds[recordName]
          || hash?.capacityBundle !== recordCapacityBundles[recordName]
          || (Object.hasOwn(hash || {}, 'authorityKey')
            && hash.authorityKey !== expectedKey)) {
        fail(`Authority Store v2 ${recordName} record differs`);
      }
    }
    if (!/^[0-9a-f]{64}$/u.test(
          fixture.operationRecords?.creation?.hash?.creationSha256 || '')
        || fixture.operationRecords?.creationTerminal?.hash?.terminalEnvelope
          !== '010000000d00000000000000000103000000'
        || fixture.operationRecords?.creationTerminal?.hash?.terminalEnvelopeSha256
          !== 'ce55bfa12e48da832d17470e81f80282667be2d06b19625f14a3df8138f66fcd'
        || fixture.operationRecords?.creationTerminal?.hash?.sourceNodeRid !== 'node-a'
        || fixture.operationRecords?.creationTerminal?.hash?.sourceNodeGeneration !== '7'
        || fixture.operationRecords?.creationTerminal?.hash?.operationIdHigh !== '0'
        || fixture.operationRecords?.creationTerminal?.hash?.operationIdLow !== '1'
        || !positiveDecimal(
          fixture.operationRecords?.creationTerminal?.hash?.expiresAtUnixMs)
        || !/^[0-9a-f]{64}$/u.test(
          fixture.operationRecords?.aggregate?.hash?.inventoryDigest || '')
        || !positiveDecimal(
          fixture.operationRecords?.aggregate?.hash?.aggregateGeneration)) {
      fail('Authority Store v3 binary record field encoding differs');
    }

    const entry = fixture.entryClaim || {};
    const entryDigest = crypto.createHash('sha256')
      .update(Buffer.from(entry.spotId || '', 'utf8')).digest('hex');
    const entryUuid = (entry.spotId || '').slice(
      (entry.spotId || '').lastIndexOf('-entry-') + 7);
    if (!exactFieldMap(
          'authority-store-v3/entryClaim', entry.hashFields, entry.hash)
        || JSON.stringify(entry.hashFields) !== JSON.stringify([
          'state', 'spotId', 'descriptorKey',
          'descriptorLifecycleGeneration', 'ownerId',
          'ownerLeaseGeneration',
        ])
        || !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/u
          .test(entryUuid)
        || entry.spotIdSha256 !== entryDigest
        || entry.key !== `${physicalBase}:entry-spot-id:${entryDigest}`
        || entry.hash?.state !== 'Claimed'
        || entry.hash?.spotId !== entry.spotId
        || !positiveDecimal(entry.hash?.descriptorLifecycleGeneration)
        || !positiveDecimal(entry.hash?.ownerLeaseGeneration)) {
      fail('Authority Store v3 Entry Spot claim differs');
    }

    const history = fixture.historyEncoding || {};
    if (history.revisionFieldSeparator !== ':'
        || history.fullSnapshotDeletedValue !== '0'
        || history.tombstoneDeletedValue !== '1'
        || JSON.stringify(history.fullSnapshotSuffixes)
          !== JSON.stringify(['deleted', ...expectedCurrentFields])
        || JSON.stringify(history.reservedFullSnapshotSuffixes)
          !== JSON.stringify(['deleted', ...expectedReservedFields])
        || JSON.stringify(history.tombstoneSuffixes)
          !== JSON.stringify(['deleted', 'authorityKey'])) {
      fail('Authority Store v2 history field encoding differs');
    }

    const reserveVersion = BigInt(
      fixture.reserve?.currentHash?.storeVersion ?? '-1');
    const commitVersion = BigInt(
      fixture.commit?.currentHash?.storeVersion ?? '-1');
    const preserveVersion = BigInt(
      fixture.preserve?.currentHash?.storeVersion ?? '-1');
    const newOwnerVersion = BigInt(
      fixture.newOwner?.currentHash?.storeVersion ?? '-1');
    const deleteVersion = BigInt(
      fixture.delete?.consumedStoreRevision ?? '-1');
    if (fixture.missing?.kind !== 'Missing'
        || Object.hasOwn(fixture.missing || {}, 'storeVersion')
        || fixture.reserve?.expectation !== 'Missing'
        || fixture.reserve?.result !== 'Reserved'
        || fixture.commit?.expectation?.storeVersion
          !== fixture.reserve?.currentHash?.storeVersion
        || fixture.commit?.result !== 'Committed'
        || fixture.preserve?.expectation?.storeVersion
          !== fixture.commit?.currentHash?.storeVersion
        || fixture.newOwner?.expectation?.storeVersion
          !== fixture.preserve?.currentHash?.storeVersion
        || fixture.delete?.expectation?.storeVersion
          !== fixture.newOwner?.currentHash?.storeVersion
        || fixture.delete?.result !== 'Deleted'
        || fixture.delete?.tombstone !== true
        || !(reserveVersion < commitVersion
          && commitVersion < preserveVersion
          && preserveVersion < newOwnerVersion
          && newOwnerVersion < deleteVersion)) {
      fail('Authority Store v2 CAS version transition differs');
    }
    if (fixture.scan?.indexMember !== authorityHex
        || fixture.scan?.revisionZsetScore !== 0
        || fixture.scan?.pageSizeMaximum !== 1000
        || fixture.scan?.encodedPageBytesMaximum !== 4 * 1024 * 1024
        || fixture.scan?.cursorBytesMaximum !== 4096
        || !/^[0-9a-f]{16}$/u.test(
          fixture.scan?.watermarkRevisionHex || '')
        || fixture.generationExhausted?.counterAtMaximum
          !== serviceWireSchema.authorityStoreGenerationProfile
            .generationMaximum
        || fixture.generationExhausted?.result !== 'GenerationExhausted'
        || fixture.generationExhausted?.retriable !== false
        || fixture.generationExhausted?.rowIndexAndCounterMutationCount !== 0) {
      fail('Authority Store v2 bounds or exhaustion contract differs');
    }
    continue;
  }
  if (name === 'authority-store-v1.json') {
    const expectedKey = canonicalAuthorityKey(fixture.keyContract);
    if (!expectedKey || fixture.keyContract?.authorityKey !== expectedKey) {
      fail('Authority Store fixture key differs from authority-key-v1');
    }
    const authorityBytes = Buffer.from(expectedKey || '', 'utf8');
    const authorityHex = authorityBytes.toString('hex');
    const authorityDigest = crypto.createHash('sha256')
      .update(authorityBytes).digest('hex');
    const physicalBase = 'P:{zlink-location-v1}';
    if (fixture.prefixRules?.literalHashTag !== '{zlink-location-v1}'
        || fixture.prefixRules?.schemaKey !== `${physicalBase}:schema`
        || fixture.prefixRules?.schemaFields?.format
          !== 'location-authority-hybrid-v1'
        || fixture.keyContract?.authorityKeyHex !== authorityHex
        || fixture.keyContract?.authorityKeySha256 !== authorityDigest
        || fixture.keyContract?.currentKey
          !== `${physicalBase}:authority:current:${authorityDigest}`
        || fixture.keyContract?.historyKey
          !== `${physicalBase}:authority:history:${authorityDigest}`
        || fixture.keyContract?.historyRevisionKey
          !== `${physicalBase}:authority:history-revisions:${authorityDigest}`
        || fixture.keyContract?.indexKey
          !== `${physicalBase}:authority:key-index`) {
      fail('Authority Store fixture hybrid physical key schema differs');
    }
    if (Object.hasOwn(fixture, 'newObject')) {
      fail('Authority Store fixture must not expose Missing authority CAS');
    }
    const capacity = fixture.capacityBuckets || {};
    const capacitySegment = value =>
      `${Buffer.byteLength(String(value), 'utf8')}:${String(value)}`;
    const expectedNodeBucket =
      capacitySegment(capacity.descriptorKey)
      + capacitySegment(capacity.descriptorLifecycleGeneration);
    const expectedTypeBucket =
      expectedNodeBucket
      + capacitySegment(capacity.objectKind)
      + capacitySegment(capacity.stableType);
    const expectedUnicodeTypeBucket =
      expectedNodeBucket
      + capacitySegment(capacity.objectKind)
      + capacitySegment(capacity.unicodeStableType);
    if (capacity.segmentLengthUnit !== 'UTF-8 bytes'
        || capacity.node !== expectedNodeBucket
        || capacity.type !== expectedTypeBucket
        || capacity.unicodeType !== expectedUnicodeTypeBucket
        || !['actor', 'user_spot', 'instance_spot'].includes(
          capacity.objectKind)) {
      fail('Authority Store fixture capacity bucket encoding differs');
    }
    const history = fixture.historyEncoding || {};
    const expectedHistorySuffixes = [
      'deleted', ...fixture.currentHashFields,
    ];
    if (history.revisionFieldSeparator !== ':'
        || history.fullSnapshotDeletedValue !== '0'
        || history.tombstoneDeletedValue !== '1'
        || JSON.stringify(history.fullSnapshotSuffixes)
          !== JSON.stringify(expectedHistorySuffixes)
        || JSON.stringify(history.tombstoneSuffixes)
          !== JSON.stringify(['deleted', 'authorityKey'])) {
      fail('Authority Store fixture history field encoding differs');
    }
    for (const transition of ['reserve', 'commit', 'preserve', 'newOwner']) {
      validateAuthorityHash(name, fixture, fixture[transition]?.currentHash);
    }
    const reserveVersion = BigInt(
      fixture.reserve?.currentHash?.storeVersion ?? '-1');
    const commitVersion = BigInt(
      fixture.commit?.currentHash?.storeVersion ?? '-1');
    const preserveVersion = BigInt(
      fixture.preserve?.currentHash?.storeVersion ?? '-1');
    const newOwnerVersion = BigInt(
      fixture.newOwner?.currentHash?.storeVersion ?? '-1');
    const deleteVersion = BigInt(fixture.delete?.consumedStoreRevision ?? '-1');
    if (fixture.missing?.kind !== 'Missing'
        || Object.hasOwn(fixture.missing || {}, 'storeVersion')
        || fixture.reserve?.expectation !== 'Missing'
        || fixture.reserve?.result !== 'Reserved'
        || fixture.reserve?.currentHash?.allocationState !== 'pending'
        || fixture.commit?.expectation?.storeVersion
          !== fixture.reserve?.currentHash?.storeVersion
        || fixture.commit?.result !== 'Committed'
        || fixture.commit?.currentHash?.allocationState !== 'active'
        || fixture.preserve?.expectation?.kind !== 'Found'
        || fixture.preserve?.expectation?.storeVersion
          !== fixture.commit?.currentHash?.storeVersion
        || fixture.newOwner?.expectation?.kind !== 'Found'
        || fixture.newOwner?.expectation?.storeVersion
          !== fixture.preserve?.currentHash?.storeVersion
        || fixture.delete?.expectation?.kind !== 'Found'
        || fixture.delete?.expectation?.storeVersion
          !== fixture.newOwner?.currentHash?.storeVersion
        || fixture.delete?.result !== 'Deleted'
        || fixture.delete?.tombstone !== true
        || !(reserveVersion < commitVersion
          && commitVersion < preserveVersion
          && preserveVersion < newOwnerVersion
          && newOwnerVersion < deleteVersion)) {
      fail('Authority Store fixture CAS version transition differs');
    }
    const created = fixture.commit?.currentHash || {};
    const preserved = fixture.preserve?.currentHash || {};
    const replaced = fixture.newOwner?.currentHash || {};
    if (created.objectGeneration !== preserved.objectGeneration
        || created.objectGeneration !== replaced.objectGeneration
        || created.authorityOwnerGeneration !== preserved.authorityOwnerGeneration
        || BigInt(replaced.authorityOwnerGeneration || '0')
          <= BigInt(preserved.authorityOwnerGeneration || '0')
        || fixture.generationExhausted?.counterAtMaximum
          !== serviceWireSchema.authorityStoreGenerationProfile.generationMaximum
        || fixture.generationExhausted?.result !== 'GenerationExhausted'
        || fixture.generationExhausted?.retriable !== false
        || fixture.generationExhausted?.rowIndexAndCounterMutationCount !== 0) {
      fail('Authority Store fixture generation transition differs');
    }
    if (fixture.scan?.indexMember !== authorityHex
        || fixture.scan?.revisionZsetScore !== 0
        || fixture.scan?.pageSizeMaximum !== 1000
        || fixture.scan?.encodedPageBytesMaximum !== 4 * 1024 * 1024
        || fixture.scan?.cursorBytesMaximum !== 4096
        || !/^[0-9a-f]{16}$/u.test(
          fixture.scan?.watermarkRevisionHex || '')) {
      fail('Authority Store fixture snapshot scan schema differs');
    }
    continue;
  }
  if (name === 'actor-location-v2.json') {
    const expectedKey = canonicalAuthorityKey(fixture.keyContract);
    if (!expectedKey || fixture.keyContract?.authorityKey !== expectedKey
        || fixture.row?.key !== expectedKey) {
      fail('Actor authority fixture key differs from authority-key-v1');
    }
    validateAuthorityHash(name, fixture, fixture.row?.hash);
    continue;
  }
  const payload = validateHashRow(name, fixture.row);
  if (!payload) continue;
  validateDescriptorPayload(name, payload);
  const identityFields = {
    'actor-location-v2.json': ['MeshName', 'ActorId'],
    'client-server-server-descriptor-v1.json': ['ChannelName', 'ServerRid'],
    'fanout-publisher-descriptor-v1.json': ['ChannelName', 'PublisherRid'],
    'mesh-node-descriptor-v1.json': ['MeshName', 'Rid'],
  };
  const fields = identityFields[name];
  if (!fields || fields.some(field => typeof payload[field] !== 'string')) {
    fail(`Redis fixture embedded identity is missing: ${name}`);
  } else if (fixture.row.key !== descriptorKey(...fields.map(field => payload[field]))) {
    fail(`Redis fixture key differs from embedded identity: ${name}`);
  }
  validateHashFields(name, fixture, fixture.row.hash);
  if (name === 'mesh-node-descriptor-v1.json') {
    const descriptorDigest = crypto.createHash('sha256')
      .update(Buffer.from(fixture.row.key, 'utf8')).digest('hex');
    const ownerDigest = crypto.createHash('sha256')
      .update(Buffer.from(payload.OwnerId, 'utf8')).digest('hex');
    const ownerTokenDigest = crypto.createHash('sha256')
      .update(Buffer.from(
        `${payload.OwnerId}\u0000${payload.LeaseGeneration}`, 'utf8'))
      .digest('hex');
    const physicalBase = 'P:{zlink-location-v3}';
    const entrySpotId = fixture.entrySpotIdentityClaim?.hash?.spotId;
    const entrySpotDigest = typeof entrySpotId === 'string'
      ? crypto.createHash('sha256')
        .update(Buffer.from(entrySpotId, 'utf8')).digest('hex')
      : null;
    if (fixture.physicalKeys?.descriptorKeySha256 !== descriptorDigest
        || fixture.physicalKeys?.ownerTokenSha256 !== ownerTokenDigest
        || fixture.physicalKeys?.descriptor
          !== `${physicalBase}:descriptor:mesh:${descriptorDigest}`
        || fixture.physicalKeys?.admission
          !== `${physicalBase}:descriptor-admission:mesh:${descriptorDigest}`
        || fixture.physicalKeys?.descriptorIndex
          !== `${physicalBase}:descriptor:mesh:index`
        || fixture.physicalKeys?.descriptorOwnerIndex
          !== `${physicalBase}:descriptor:mesh:owner:${ownerTokenDigest}`
        || fixture.physicalKeys?.ownerLease
          !== `${physicalBase}:owner-lease:${ownerDigest}`
        || !entrySpotDigest
        || fixture.physicalKeys?.entrySpotIdentityClaim
          !== `${physicalBase}:entry-spot-id:${entrySpotDigest}`
        || JSON.stringify(fixture.ownerLeaseHashFields)
          !== JSON.stringify(['ownerId', 'generation', 'expiresAt'])
        || JSON.stringify(fixture.admissionHashFields) !== JSON.stringify([
          'descriptorKey', 'descriptorRevision', 'lifecycleGeneration',
          'ownerId', 'ownerLeaseGeneration', 'objectRole', 'runtimeState',
          'applicationVersion', 'capabilities', 'actorLimit',
          'spotLimit', 'activationConcurrencyLimit', 'entrySpotId',
          'immutableDigest',
        ])
        || JSON.stringify(fixture.entrySpotIdentityClaim?.hashFields)
          !== JSON.stringify([
            'state', 'spotId', 'descriptorKey',
            'descriptorLifecycleGeneration', 'ownerId',
            'ownerLeaseGeneration',
          ])
        || fixture.entrySpotIdentityClaim?.hash?.state !== 'Claimed'
        || fixture.entrySpotIdentityClaim?.hash?.descriptorKey
          !== fixture.row.key
        || fixture.entrySpotIdentityClaim?.hash?.descriptorLifecycleGeneration
          !== String(payload.LifecycleGeneration)
        || fixture.entrySpotIdentityClaim?.hash?.ownerId !== payload.OwnerId
        || fixture.entrySpotIdentityClaim?.hash?.ownerLeaseGeneration
          !== String(payload.LeaseGeneration)) {
      fail('MeshNode descriptor hybrid physical schema differs');
    }
  }
  const payloadLeaseGeneration = name === 'mesh-node-descriptor-v1.json'
    ? payload.LeaseGeneration
    : payload.OwnerLeaseGeneration;
  if (payload.OwnerId !== fixture.row.hash.owner
      || String(name === 'mesh-node-descriptor-v1.json'
        ? payload.LifecycleGeneration
        : payloadLeaseGeneration) !== fixture.row.hash.gen) {
    fail(`Redis descriptor fixture owner lease token differs: ${name}`);
  }
}

const amendedObjectSemanticFixtures = [
  ['cpp', ['04-spots.ko.md', '05-actors.ko.md'], [
    'global ActorId', 'global SpotId', 'send(actor_id_t actor_id',
    'spot_send_call_t send_to_spot(spot_id_t target',
    'spot_request_call_t request_to_spot(',
    '`instance_spot()`은 [stable type]',
    '`spot_manager_t`는 User Spot만 생성한다.',
    'distinct Instance Spot type이 0개이면',
    'class actor_manager_t', 'class spot_manager_t',
    'object_generation() const noexcept', 'actor-free lifecycle',
    'fresh incarnation으로 자동 bind하지 않는다.',
  ]],
  ['dotnet', ['05-spots.ko.md', '06-actors.ko.md', '07-stream-session.ko.md'], [
    'ActorId는 Location Store transaction domain 전체에서 유일한 logical ID',
    'Entry·User·Instance SpotId는 UTF-8 encoded 크기 1..255 bytes의 global string key다.',
    'SendToActor<TMessage>(',
    'IZLinkSpotSendCall SendToSpot<TMessage>(string spotId',
    'IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId',
    'IZLinkSpotSendCall InstanceSpot();',
    'IZLinkSpotRequestCall InstanceSpot();',
    '`IZLinkSpotManager`는 User Spot의 명시적 create·get-or-create, resolve와 exact close만 제공한다.',
    'Instance Spot type이 하나일 때만',
    'public interface IZLinkActorManager', 'public interface IZLinkSpotManager',
    'ActorRef.ObjectGeneration', 'SpotRef.ObjectGeneration', 'actor-free lifecycle interface',
    '다른 ref를 찾아 같은 bind operation을 hidden retry하지 않는다.',
  ]],
  ['java', ['actors.ko.md', 'spots.ko.md', 'stream-session.ko.md'], [
    'ActorId는 UTF-8 1..255 bytes의 global logical ID',
    'Entry·User·Instance SpotId는 UTF-8 encoded 크기 1..255 bytes의 `String`인 global logical ID',
    'sendToActor(java.lang.String, java.lang.Object)',
    'ZLinkSpotSendCall sendToSpot(java.lang.String, java.lang.Object)',
    'ZLinkSpotRequestCall requestToSpot(java.lang.String, java.lang.Object)',
    'ZLinkSpotSendCall instanceSpot();',
    'ZLinkSpotRequestCall instanceSpot();',
    'Spot manager는 User Spot 전용이다.',
    'serving 가능한 distinct Instance type이',
    '정확히 하나일 때만 그 type을 사용한다.',
    'public interface systems.zlink.framework.actors.ZLinkActorManager',
    'public interface systems.zlink.framework.spots.ZLinkSpotManager',
    'ActorRef.objectGeneration()', 'SpotRef.objectGeneration()',
    '새 ref를 찾아 hidden retry하지 않는다.',
  ]],
  ['kotlin', ['README.ko.md', 'actors.ko.md', 'spots.ko.md', 'stream-session.ko.md'], [
    'global ActorId·SpotId', 'ID-only 일반',
    'ZLinkKotlinActorManager.create(actorId, actorType)',
    'ZLinkSpotManager.create(spotType)',
    'Manager는 Instance Spot',
    'create/get-or-create를 제공하지 않는다.',
    'Spot 전용 send/request call에서 `instanceSpot()` 또는',
    '`instanceSpot(stableType)`을 명시한 경우에만',
    'serving Instance type이 distinct',
    'value 하나일 때만 그 type을 자동 선택한다.',
    'ActorRef(actorId, objectGeneration, meshName, nodeRid)',
    'SpotRef(spotId, objectGeneration, meshName, nodeRid)',
    'Framework는 hidden retry나 local fallback을 수행하지 않는다.',
  ]],
  ['node', ['01-foundation-configuration.ko.md', '04-spots.ko.md', '05-actors.ko.md'], [
    '`ActorId`는 Location Store transaction domain 전체에서 유일한 logical ID',
    'Entry·User·Instance SpotId는 UTF-8 encoded 크기 1..255 bytes의 global string key다.',
    'sendToActor(actorId: ActorId',
    'sendToSpot(spotId: SpotId, message: unknown): ZLinkSpotSendCall',
    'requestToSpot(spotId: SpotId, request: unknown): ZLinkSpotRequestCall',
    'instanceSpot(): this;',
    'Instance Spot에는 manager create·get-or-create를 제공하지 않는다.',
    'distinct Instance type이 하나일 때',
    'export interface ZLinkActorManager', 'export interface ZLinkSpotManager',
    'readonly objectGeneration: bigint', 'export interface ZLinkInstanceSpot {',
    'Framework는 current ref를 다시 찾아 다른 incarnation을 닫지 않는다.',
  ]],
];
for (const [language, files, required] of amendedObjectSemanticFixtures) {
  const directory = path.posix.join(
    'framework/doc/framework/common/spec/server/languages', language, 'interfaces');
  const source = files.map(name => fs.readFileSync(path.join(root, directory, name), 'utf8')).join('\n');
  for (const fragment of required) {
    if (!source.includes(fragment)) {
      fail(`${language} amended object contract is missing: ${fragment}`);
    }
  }
  if (/(?:OwnerLeaseFencingMargin|ownerLeaseFencingMargin|owner_lease_fencing_margin)[^\n]{0,80}(?:1초|FromSeconds\(1\)|1000)/u.test(source)) {
    fail(`${language} exact contract retains a one-second owner lease fencing margin`);
  }
  const allExactSource = markdownDocumentsUnder(directory)
    .map(relative => fs.readFileSync(path.join(root, relative), 'utf8')).join('\n');
  if (/(?:location generation|activation epoch)/iu.test(allExactSource)) {
    fail(`${language} exact contract retains a removed location generation or activation epoch name`);
  }
}

const javaSpotsSource = fs.readFileSync(path.join(
  root,
  'framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md'), 'utf8');
const javaInstanceSpot = javaSpotsSource.match(
  /public interface ZLinkInstanceSpot\s*\{([\s\S]*?)\n\}/u)?.[1] || '';
if (!javaInstanceSpot || /ZLinkActor|ActorLifecycle/u.test(javaInstanceSpot)) {
  fail('java Instance Spot must retain an actor-free exact interface');
}
const nodeSpotsSource = fs.readFileSync(path.join(
  root,
  'framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md'), 'utf8');
const nodeInstanceSpot = nodeSpotsSource.match(
  /export interface ZLinkInstanceSpot\s*\{([\s\S]*?)\n\}/u)?.[1] || '';
if (!nodeInstanceSpot || /ZLinkActor|ActorLifecycle/u.test(nodeInstanceSpot)) {
  fail('node Instance Spot must retain an actor-free exact interface');
}
const kotlinExactSource = markdownDocumentsUnder(
  'framework/doc/framework/common/spec/server/languages/kotlin/interfaces')
  .map(relative => fs.readFileSync(path.join(root, relative), 'utf8')).join('\n');
if (/ZLinkSuspendingInstanceSpot\s*</u.test(kotlinExactSource)
    || /ZLinkInstanceSpotActor/u.test(kotlinExactSource)) {
  fail('Kotlin exact contract adds an actor-bearing Instance Spot wrapper');
}

const removedMessageContextDeclaration = new RegExp(
  String.raw`(?:class|interface|struct|record|data\s+class)\s+(?:ZLinkSendContext|ZLinkRequestContext|ZLinkPublishContext|ZLinkSpotActorMessageContext|spot_packet_context_t|stream_dispatch_context_t)\b`,
  'u');
for (const language of ['dotnet', 'java', 'kotlin', 'node', 'cpp']) {
  const directory = path.posix.join(
    'framework/doc/framework/common/spec/server/languages', language, 'interfaces');
  const source = markdownDocumentsUnder(directory)
    .map(relative => fs.readFileSync(path.join(root, relative), 'utf8')).join('\n');
  if (removedMessageContextDeclaration.test(source)) {
    fail(`${language} exact contract declares a removed message marker context`);
  }
}

const messageFollowDocuments = [
  ...markdownDocumentsUnder('framework/doc/framework/common/spec'),
  ...markdownDocumentsUnder('framework/doc/framework/common/e2e'),
  ...markdownDocumentsUnder('framework/doc/framework/common/internals'),
];
const removedMessageFollowTerms =
  /\b(?:RelocationForwardingWindow|relocationForwardingWindow(?:Ms)?|relocation_forwarding_window|ActorTransferForwardWindow|actorTransferForwardWindow(?:Ms)?|actor_transfer_forward_window|straggler_forward|mapping_evicted)\b|stale-route forwarding|forwarding mapping/iu;
for (const relative of messageFollowDocuments) {
  const source = fs.readFileSync(path.join(root, relative), 'utf8');
  if (removedMessageFollowTerms.test(source)) {
    fail(`${relative} retains terminology replaced by Message Follow`);
  }
}

if (failures.length) {
  process.stderr.write(`${failures.map(message => `FAIL: ${message}`).join('\n')}\n`);
  process.exit(1);
}
process.stdout.write(
  `FRAMEWORK DOC CONTRACTS CLEAN version=${inventory.version} languages=${expectedLanguages.length}`
  + ` exact_documents=${exactDocumentCount} exact_fences=${exactFenceCount}`
  + ` package_contract_fences=${exactFenceRoleCounts.get('package-contract')}`
  + ` application_example_fences=${exactFenceRoleCounts.get('application-example')}`
  + ` documentation_support_fences=${exactFenceRoleCounts.get('documentation-support')}`
  + ` syntax_and_example_fences=${syntaxAndExampleFenceCount}`
  + ` package_declaration_owners=${packageDeclarationOwnerCount}`
  + ` block_role_negative_mutations=${blockRoleNegativeMutationCount}`
  + ` formal_documents=${allFormalPaths.length}`
  + ` semantic_owners=${formalPaths.length} target_documents=${targetDocumentCount}`
  + ` plan_documents=${planDocumentCount} consolidated_semantics=${consolidatedSemanticCount}`
  + ` java_kotlin_negative_mutations=${javaKotlinSemanticNegativeTestCount}`
  + ` host_lifecycle_negative_mutations=${hostLifecycleContractNegativeTestCount}`
  + ` redis_fixtures=${redisFixtureCount} legacy_plan_absent=${forbiddenLegacyPlanCount}\n`);
NODE
