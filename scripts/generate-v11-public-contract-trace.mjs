#!/usr/bin/env node

// SPDX-License-Identifier: MPL-2.0

import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';

const require = createRequire(import.meta.url);
const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, '..');
const configPath = path.join(
  repositoryRoot,
  'framework/doc/contract-inventory/route-mesh-v11-public-contract-trace.config.json');
const tracePath = path.join(
  repositoryRoot,
  'framework/doc/contract-inventory/route-mesh-v11-public-contract-trace.json');
const {
  exactInterfaceBlockRole,
  exactDocumentPaths,
  headedFencedBlocks,
  publicDeclarationNames,
} = require(path.join(repositoryRoot, 'scripts/lib/framework-contract-documents.cjs'));

const sha256 = source => createHash('sha256').update(source).digest('hex');
const stableJson = value => `${JSON.stringify(value, null, 2)}\n`;
const uniqueSorted = values => [...new Set(values)]
  .sort((left, right) => left.localeCompare(right, 'en'));
const sourceLine = (source, offset) => source.slice(0, offset).split(/\r?\n/u).length;
const requiredScenarioIds = () => [
  ...Array.from({length: 74}, (_, index) => `V11-E2E-M${String(index + 1).padStart(2, '0')}`),
  ...Array.from({length: 6}, (_, index) => `V11-E2E-L${String(index + 1).padStart(2, '0')}`),
  ...Array.from({length: 38}, (_, index) => `V11-RACE-${String(index + 1).padStart(2, '0')}`),
].sort((left, right) => left.localeCompare(right, 'en'));

const compilePattern = (expression, label) => {
  try {
    return new RegExp(expression, 'u');
  } catch (error) {
    throw new Error(`${label} has an invalid regular expression ${expression}: ${error.message}`);
  }
};

const maskCode = source => {
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

const matchingDelimiter = (source, open, left, right) => {
  let depth = 0;
  for (let index = open; index < source.length; index += 1) {
    if (source[index] === left) depth += 1;
    else if (source[index] === right) {
      depth -= 1;
      if (depth === 0) return index;
    }
  }
  return -1;
};

const ownerPatternFor = tag => {
  if (tag === 'csharp') {
    return /\bpublic\s+(?:(?:sealed|abstract|readonly|partial|static)\s+)*(class|struct|interface|enum|record(?:\s+struct|\s+class)?)\s+([A-Za-z_]\w*)/gu;
  }
  if (tag === 'cpp') {
    return /(?:^|\n)\s*(?:(?:template\s*<[^\n>]+>\s*)?)(class|struct|enum\s+class|enum)\s+([A-Za-z_]\w*)\b/gmu;
  }
  if (tag === 'java') {
    return /\b(?:(?:public|protected|private|sealed|non-sealed|final|abstract|static)\s+)*(class|interface|record|enum|@interface)\s+([A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*)/gu;
  }
  if (tag === 'kotlin') {
    return /\b(?:(?:public|private|protected|internal|data|sealed|enum|value|annotation|open|abstract|final|inner)\s+)*(class|interface|object)\s+([A-Za-z_$][\w$]*)/gu;
  }
  return /\bexport\s+(?:declare\s+)?(interface|class|enum|type)\s+([A-Za-z_$][\w$]*)/gu;
};

const ownerSpans = (source, tag) => {
  const masked = maskCode(source);
  const spans = [];
  for (const match of masked.matchAll(ownerPatternFor(tag))) {
    const open = masked.indexOf('{', match.index + match[0].length);
    const semicolon = masked.indexOf(';', match.index + match[0].length);
    const semicolonTerminated = semicolon >= 0
      && semicolon < open
      && (match[1] === 'type' || (tag === 'csharp' && match[1].startsWith('record')));
    if (open < 0 || semicolonTerminated) {
      spans.push({
        close: semicolon >= 0 ? semicolon : match.index + match[0].length,
        kind: match[1].replace(/\s+/gu, '-'),
        name: match[2],
        nameEnd: match.index + match[0].length,
        open: -1,
        start: match.index,
      });
      continue;
    }
    const close = matchingDelimiter(masked, open, '{', '}');
    spans.push({
      close: close < 0 ? source.length : close,
      kind: match[1].replace(/\s+/gu, '-'),
      name: match[2],
      nameEnd: match.index + match[0].length,
      open,
      start: match.index,
    });
  }
  return spans;
};

const enclosingOwner = (spans, index) => spans
  .filter(span => span.open >= 0 && span.open < index && index < span.close)
  .sort((left, right) => (left.close - left.start) - (right.close - right.start))[0];

const primaryPackage = packages => packages[0];
const qualifyOwner = (language, packages, owner) => {
  if ((language === 'java' || language === 'kotlin') && owner.includes('.')) return owner;
  if (language === 'cpp' && owner.includes('::')) return owner;
  return `${primaryPackage(packages)}::${owner}`;
};

const normalizeUnit = source => source
  .replace(/\s+/gu, ' ')
  .replace(/\s*([(),;:{}])\s*/gu, '$1')
  .trim();

const splitParameters = source => {
  const parameters = [];
  let start = 0;
  let angle = 0;
  let square = 0;
  for (let index = 0; index <= source.length; index += 1) {
    const character = source[index];
    if (character === '<') angle += 1;
    else if (character === '>') angle = Math.max(0, angle - 1);
    else if (character === '[') square += 1;
    else if (character === ']') square = Math.max(0, square - 1);
    if ((character === ',' && angle === 0 && square === 0) || index === source.length) {
      parameters.push(source.slice(start, index).trim());
      start = index + 1;
    }
  }
  return parameters.filter(Boolean);
};

const canonicalJavaType = (source, hasParameterName = false) => {
  let value = source
    .replace(/@[A-Za-z_$][\w$]*(?:\([^)]*\))?\s*/gu, '')
    .replace(/\b(?:final|volatile|transient)\s+/gu, '')
    .replace(/\.\.\./gu, '[]')
    .trim();
  if (hasParameterName) {
    value = value.replace(/\s+[A-Za-z_$][\w$]*\s*$/u, '');
  }
  return value
    .replace(/(?:[a-z_$][\w$]*\.)+([A-Z_$][\w$]*)/gu, '$1')
    .replace(/\s+/gu, '')
    .replace(/\?extends/gu, '? extends ')
    .replace(/\?super/gu, '? super ');
};

const canonicalJavaShape = member => {
  const signature = member.normalizedSignature;
  const nameOffset = signature.indexOf(member.memberName);
  const open = signature.indexOf('(', Math.max(0, nameOffset));
  if (open < 0) return `${member.kind}|${member.memberName}|${canonicalJavaType(signature)}`;
  const close = matchingDelimiter(signature, open, '(', ')');
  if (close < 0) return `${member.kind}|${member.memberName}|${canonicalJavaType(signature)}`;
  const prefix = signature.slice(0, nameOffset)
    .replace(/^(?:(?:public|protected|private|abstract|default|static|final|native|synchronized|strictfp)\s+)+/u, '')
    .replace(/^<[^>]+>\s*/u, '')
    .trim();
  const returnType = member.kind === 'constructor'
    ? '<constructor>'
    : canonicalJavaType(prefix.split(/\s+/u).at(-1) || prefix);
  const parameters = splitParameters(signature.slice(open + 1, close))
    .map(parameter => canonicalJavaType(parameter, /\s+[A-Za-z_$][\w$]*\s*$/u.test(parameter)));
  return `${member.kind}|${returnType}|${parameters.join(',')}`;
};

const canonicalMemberIdentity = member => {
  if (member.language !== 'java') return member.identity;
  const namespaceSeparator = member.ownerIdentity.lastIndexOf('::');
  const owner = namespaceSeparator >= 0
    ? member.ownerIdentity.slice(namespaceSeparator + 2)
    : member.ownerIdentity.slice(member.ownerIdentity.lastIndexOf('.') + 1);
  const shape = canonicalJavaShape(member);
  return `${member.language}|${primaryPackage(member.packages)}|${owner}.${member.memberName}`
    + `#${sha256(shape).slice(0, 16)}`;
};

const memberNameFromUnit = (unit, tag) => {
  const stripped = unit
    .replace(/^\s*(?:public|protected|private)\s*:\s*/u, '')
    .replace(/^(?:@[A-Za-z_$][\w$]*(?:\([^)]*\))?\s*)+/u, '')
    .trim();
  if (/^(?:return|throw|co_return|break|continue|case|default)\b/u.test(stripped)) return undefined;
  if (/\b(?:class|struct|interface|record|enum|namespace|package|import)\b/u.test(stripped)
      && !/\bdelegate\b/u.test(stripped)) return undefined;
  if (/^(?:using|typealias|export\s+(?:declare\s+)?type)\b/u.test(stripped)) return undefined;
  const operator = /\boperator\s*([^\s(]+)\s*\(/u.exec(stripped);
  if (operator) return `operator${operator[1]}`;
  const callable = /(~?[A-Za-z_$][\w$]*)\??\s*(?:<[^<>;{}()]*>)?\s*\(/u.exec(stripped);
  if (callable) return callable[1];
  if (tag === 'ts' || tag === 'typescript') {
    if (/^declare\s+(?:const|let|var)\b/u.test(stripped)) return undefined;
    const exportedValue = /^(?:export\s+)?(?:declare\s+)?(?:const|let|var)\s+([A-Za-z_$][\w$]*)/u.exec(stripped);
    if (exportedValue) return exportedValue[1];
    const typescriptComputedProperty =
      /^(?:(?:public|private|protected|static|readonly|abstract|declare)\s+)*\[([A-Za-z_$][\w$]*)\]\??\s*:/u.exec(stripped);
    if (typescriptComputedProperty) return `[${typescriptComputedProperty[1]}]`;
    const typescriptProperty = /^(?:(?:public|private|protected|static|readonly|abstract|declare)\s+)*([A-Za-z_$][\w$]*)\??\s*:/u.exec(stripped);
    if (typescriptProperty) return typescriptProperty[1];
  }
  const beforeInitializer = stripped.replace(/\s*=.*$/su, '').replace(/;\s*$/u, '').trim();
  const identifiers = [...beforeInitializer.matchAll(/[A-Za-z_$][\w$]*/gu)];
  return identifiers.at(-1)?.[0];
};

const signatureFacets = (kind, name, signature, ownerName, language, tag) => {
  const facets = [];
  const simpleOwner = ownerName?.slice(Math.max(ownerName.lastIndexOf('.'), ownerName.lastIndexOf(':')) + 1);
  if (kind === 'constructor' || (name && simpleOwner && (name === simpleOwner || name === `~${simpleOwner}`))) {
    facets.push('constructor');
  }
  if (/\b(?:template\s*<|extends\b|where\b|requires\b|reified\b|\bin\s+[A-Z]|\bout\s+[A-Z])/u.test(signature)) {
    facets.push('generic-bound');
  }
  if (kind === 'callback'
      || /(?:Handler|Callback|Listener|Observer|delegate|std::function|Function\d*<|\([^)]*\)\s*->)/u.test(signature)) {
    facets.push('callback');
  }
  if (kind === 'enum-value') facets.push('enum-value');
  if (kind === 'extension' || (language === 'kotlin' && tag === 'kotlin'
      && /\bfun\s+(?:<[^>]+>\s*)?[A-Za-z_$][\w$]*(?:<[^>]+>)?\??\s*\./u.test(signature))) {
    facets.push('extension');
  }
  if (/\b(?:Class|Method|Property|Parameter)Decorator\b/u.test(signature)) facets.push('decorator');
  if (/\b(?:InjectionToken|DI token|ServiceCollection|Bean|Provider)\b/u.test(signature)
      || /(?:^|[^A-Za-z])(?:TOKEN|Token)\b/u.test(signature)) {
    facets.push('di-token');
  }
  return uniqueSorted(facets);
};

const memberRecord = ({
  block,
  categories,
  disposition,
  document,
  kind,
  language,
  line,
  name,
  owner,
  packages,
  signature,
}) => {
  const normalized = normalizeUnit(signature);
  const signatureSha256 = sha256(normalized);
  const ownerName = owner?.name || '<package>';
  const qualifiedOwner = qualifyOwner(language, packages, ownerName);
  const record = {
    categoryIds: categories,
    codeBlockId: block.id,
    disposition,
    exactInterface: document,
    facets: signatureFacets(kind, name, normalized, ownerName, language, block.tag),
    identity: `${language}|${primaryPackage(packages)}|${qualifiedOwner}.${name}#${signatureSha256.slice(0, 16)}`,
    kind,
    language,
    line,
    memberName: name,
    normalizedSignature: normalized,
    ownerIdentity: qualifiedOwner,
    packages,
    signatureSha256,
  };
  record.canonicalIdentity = canonicalMemberIdentity(record);
  record.canonicalSignatureSha256 = record.language === 'java'
    ? sha256(canonicalJavaShape(record))
    : signatureSha256;
  return record;
};

const isPublicSourceMember = (signature, owner, tag) => {
  if (tag !== 'csharp') return true;
  const stripped = signature
    .replace(/^(?:\s*\[[^\]]+\]\s*)+/u, '')
    .trim();
  if (/^(?:private|protected|internal)\b/u.test(stripped)) return false;
  if (/^public\b/u.test(stripped)) return true;
  return owner?.kind === 'interface' || owner?.kind?.includes('enum');
};

const extractMembers = context => {
  const {
    block,
    categories,
    disposition = 'unclassified',
    document,
    language,
    packages,
    publicSourceOnly = false,
  } = context;
  const source = block.source;
  const masked = maskCode(source);
  const spans = ownerSpans(source, block.tag);
  const members = [];
  const seen = new Set();
  const add = (kind, name, signature, offset, owner = enclosingOwner(spans, offset)) => {
    if (!name || !signature.trim()) return;
    if (publicSourceOnly && !isPublicSourceMember(signature, owner, block.tag)) return;
    const normalized = normalizeUnit(signature);
    const key = `${kind}\0${owner?.name || '<package>'}\0${normalized}`;
    if (seen.has(key)) return;
    seen.add(key);
    members.push(memberRecord({
      block,
      categories,
      disposition,
      document,
      kind,
      language,
      line: block.startLine + sourceLine(source, offset) - 1,
      name,
      owner,
      packages,
      signature,
    }));
  };

  for (let index = 0; index < masked.length; index += 1) {
    if (masked[index] !== ';') continue;
    const owner = enclosingOwner(spans, index);
    let start = masked.lastIndexOf(';', index - 1) + 1;
    if (owner?.open >= 0) start = Math.max(start, owner.open + 1);
    let signature = source.slice(start, index + 1).trim();
    if (signature.length === 0 || signature.length > 12000) continue;
    signature = signature
      .replace(/^(?:\s*\}\s*)+/u, '')
      .replace(/^\s*(?:public|protected|private)\s*:\s*/u, '');
    const signatureMask = maskCode(signature);
    const openBraceCount = [...signatureMask.matchAll(/\{/gu)].length;
    const closeBraceCount = [...signatureMask.matchAll(/\}/gu)].length;
    if (openBraceCount > closeBraceCount) continue;
    const topLevelDeclaration = /^(?:(?:public|protected|private|internal|static|final|abstract|default|virtual|override|inline|constexpr|explicit|friend|export|declare|template|suspend|operator|fun)\b|~?[A-Za-z_$][\w$:<>,?*&\s]*\()/u.test(signature);
    if (!owner && !topLevelDeclaration) continue;
    if (/^(?:get|set|init|add|remove)\s*;/u.test(signature)) continue;
    const name = memberNameFromUnit(signature, block.tag);
    if (!name) continue;
    let kind = /\bdelegate\b/u.test(signature) || /std::function/u.test(signature)
      ? 'callback'
      : signature.includes('(') ? 'method' : 'field';
    const simpleOwner = owner?.name.slice(owner.name.lastIndexOf('.') + 1);
    if (simpleOwner && (name === simpleOwner || name === `~${simpleOwner}`)) kind = 'constructor';
    if (owner?.kind.includes('enum')
        && /\b(?:static\s+final|const|constexpr)\b/u.test(signature)
        && !signature.includes('(')) kind = 'enum-value';
    add(kind, name, signature, start, owner);
  }

  const propertyPattern = block.tag === 'csharp'
    ? /(?:^|\n)\s*(?:public|protected|internal)\s+[^;{}\n=]+?\s+([A-Za-z_$][\w$]*)\s*\{\s*(?:get|set|init)\s*;/gmu
    : /$a/gu;
  for (const match of masked.matchAll(propertyPattern)) {
    add('property', match[1], source.slice(match.index, masked.indexOf('}', match.index) + 1), match.index);
  }

  if (block.tag === 'kotlin') {
    const functionPattern = /\bfun\s+(?:<[^>\n]+>\s*)?(?:([A-Za-z_$][\w$]*(?:<[^>\n]+>)?\??)\s*\.\s*)?([A-Za-z_$][\w$]*)\s*\(/gmu;
    for (const match of masked.matchAll(functionPattern)) {
      const open = masked.indexOf('(', match.index);
      const close = matchingDelimiter(masked, open, '(', ')');
      if (close < 0) continue;
      let end = close + 1;
      while (end < masked.length && masked[end] !== '\n' && masked[end] !== '=' && masked[end] !== '{') end += 1;
      const kind = match[1] ? 'extension' : 'method';
      add(kind, match[2], source.slice(match.index, end), match.index);
    }
    const propertyPatternKotlin = /(?:^|\n)\s*(?:(?:public|private|protected|internal|const|lateinit|override)\s+)*(?:val|var)\s+([A-Za-z_$][\w$]*)[^\n]*/gmu;
    for (const match of masked.matchAll(propertyPatternKotlin)) {
      add('property', match[1], source.slice(match.index, match.index + match[0].length), match.index);
    }
  }

  if (block.tag !== 'kotlin') {
    const ownerOpenOffsets = new Set(spans.filter(span => span.open >= 0).map(span => span.open));
    for (let index = 0; index < masked.length; index += 1) {
      if (masked[index] !== '{' || ownerOpenOffsets.has(index)) continue;
      const owner = enclosingOwner(spans, index);
      if (!owner) continue;
      const precedingBody = masked.slice(owner.open + 1, index);
      const depth = [...precedingBody].reduce((value, character) => {
        if (character === '{') return value + 1;
        if (character === '}') return value - 1;
        return value;
      }, 0);
      if (depth !== 0) continue;
      const previousSemicolon = masked.lastIndexOf(';', index - 1);
      const previousClose = masked.lastIndexOf('}', index - 1);
      const start = Math.max(owner.open, previousSemicolon, previousClose) + 1;
      const signatureMask = masked.slice(start, index);
      const parenthesisDepth = [...signatureMask].reduce((value, character) => {
        if (character === '(') return value + 1;
        if (character === ')') return value - 1;
        return value;
      }, 0);
      if (parenthesisDepth !== 0) continue;
      const signature = source.slice(start, index).trim();
      if (!signature.includes('(')
          || /^(?:if|for|while|switch|catch|lock|using|checked|unchecked)\b/u.test(signature)
          || /\b(?:class|struct|interface|record|enum|namespace)\b/u.test(signature)) {
        continue;
      }
      const name = memberNameFromUnit(signature, block.tag);
      if (!name) continue;
      const simpleOwner = owner.name.slice(owner.name.lastIndexOf('.') + 1);
      const kind = name === simpleOwner || name === `~${simpleOwner}` ? 'constructor' : 'method';
      add(kind, name, signature, start, owner);
    }
  }

  for (const span of spans) {
    const headerEnd = span.open >= 0 ? span.open : span.close;
    const header = source.slice(span.start, headerEnd);
    if (/\brecord\b/u.test(header) || (block.tag === 'kotlin' && header.includes('('))) {
      const open = masked.indexOf('(', span.nameEnd - span.name.length);
      if (open >= 0 && open < headerEnd) {
        const close = matchingDelimiter(masked, open, '(', ')');
        if (close >= 0 && close < headerEnd) {
          add('constructor', span.name.slice(span.name.lastIndexOf('.') + 1), source.slice(span.start, close + 1), span.start, span);
        }
      }
    }
    if (!span.kind.includes('enum') || span.open < 0) continue;
    const bodyEnd = masked.indexOf(';', span.open + 1);
    const limit = bodyEnd >= 0 && bodyEnd < span.close ? bodyEnd : span.close;
    const body = source.slice(span.open + 1, limit);
    let partStart = span.open + 1;
    let nested = 0;
    for (let offset = 0; offset <= body.length; offset += 1) {
      const character = body[offset];
      if (character === '(' || character === '{' || character === '[') nested += 1;
      else if (character === ')' || character === '}' || character === ']') nested -= 1;
      if ((character !== ',' && offset !== body.length) || nested !== 0) continue;
      const part = source.slice(partStart, span.open + 1 + offset).trim();
      const value = /^(?:@[A-Za-z_$][\w$]*(?:\([^)]*\))?\s*)*([A-Za-z_$][\w$]*)/u.exec(part)?.[1];
      if (value && !/^(?:public|private|protected|static|final)$/u.test(value)) {
        add('enum-value', value, part, partStart, span);
      }
      partStart = span.open + offset + 2;
    }
  }

  const overloads = new Map();
  for (const member of members) {
    const key = `${member.ownerIdentity}\0${member.memberName}`;
    if (!overloads.has(key)) overloads.set(key, []);
    overloads.get(key).push(member);
  }
  for (const group of overloads.values()) {
    if (new Set(group.map(member => member.signatureSha256)).size > 1) {
      for (const member of group) member.facets = uniqueSorted([...member.facets, 'overload']);
    }
  }
  return members.sort((left, right) => left.identity.localeCompare(right.identity, 'en'));
};

const packagesForDocument = (languageConfig, document) => {
  for (const rule of languageConfig.packageRules || []) {
    if (compilePattern(rule.documentPattern, `${languageConfig.id} package rule`).test(document)) {
      return rule.packages;
    }
  }
  return languageConfig.defaultPackages;
};

const ownershipFor = (config, categoryIds, languageConfig) => {
  const categories = config.categories.filter(category => categoryIds.includes(category.id));
  const expand = stage => stage.replace('{suffix}', languageConfig.implementationSuffix);
  return {
    contractTestOwners: uniqueSorted(categories.flatMap(category => category.contractTestOwners)),
    decisionIds: uniqueSorted(categories.flatMap(category => category.decisionIds)),
    e2eScenarioIds: uniqueSorted(categories.flatMap(category => category.e2eScenarioIds)),
    formalSpecOwners: uniqueSorted(categories.flatMap(category => category.formalSpecOwners)),
    implementationLedgerIds: uniqueSorted(categories.flatMap(category => category.implementationStages.map(expand))),
    packageGateIds: [languageConfig.packageGate],
  };
};

const gitText = arguments_ => execFileSync('git', arguments_, {
  cwd: repositoryRoot,
  encoding: 'utf8',
  maxBuffer: 64 * 1024 * 1024,
});

const memberOwnerNameKey = member =>
  `${member.language}\0${member.ownerIdentity}\0${member.memberName}`;

const orderOverridesForRefresh = (overrides, baselineByIdentity, targetByIdentity) =>
  overrides
    .map((override, index) => {
      const baseline = baselineByIdentity.get(override.baselineIdentity);
      const target = targetByIdentity.get(override.targetIdentity);
      const score =
        (baseline?.signatureSha256 === override.baselineSignatureSha256 ? 1 : 0)
        + (target?.signatureSha256 === override.targetSignatureSha256 ? 1 : 0);
      return {index, override, score};
    })
    .sort((left, right) => right.score - left.score || left.index - right.index)
    .map(candidate => candidate.override);

const blockSimilarity = (left, right) => {
  const tokens = source => new Set(source.match(/[A-Za-z_][A-Za-z0-9_]*/gu) || []);
  const leftTokens = tokens(left);
  const rightTokens = tokens(right);
  const intersection = [...leftTokens].filter(token => rightTokens.has(token)).length;
  const union = new Set([...leftTokens, ...rightTokens]).size;
  return union === 0 ? 1 : intersection / union;
};

const historicalSpecDocument = document => {
  const commonPrefix = 'framework/doc/framework/common/spec/';
  if (!document.startsWith(commonPrefix)) return document;
  const historicalRoot = ['framework', 'doc', 'framework', 'spec'].join('/');
  const relative = document.slice(commonPrefix.length);
  if (relative.startsWith('server/languages/')) {
    return `${historicalRoot}/${relative}`;
  }
  if (/^\d{2}-/u.test(relative)) {
    return `${historicalRoot}/server/${relative}`;
  }
  return `${historicalRoot}/${relative}`;
};

const refreshBlockRoleOverrides = config => {
  for (const rule of config.blockRoleOverrides || []) {
    const currentSource = fs.readFileSync(path.join(repositoryRoot, rule.document), 'utf8');
    const currentBlocks = headedFencedBlocks(currentSource);
    if (currentBlocks.some(block => sha256(block.source) === rule.sha256)) continue;

    let previousBlock;
    let previousPeerIndex = -1;
    for (const revision of ['HEAD', config.baseline?.revision]) {
      if (!revision) continue;
      for (const previousDocument of uniqueSorted([
        rule.document,
        historicalSpecDocument(rule.document),
      ])) {
        try {
          const previousSource = gitText(['show', `${revision}:${previousDocument}`]);
          const previousBlocks = headedFencedBlocks(previousSource);
          previousBlock = previousBlocks.find(block => sha256(block.source) === rule.sha256);
          if (previousBlock) {
            previousPeerIndex = previousBlocks
              .filter(block => block.heading === previousBlock.heading && block.tag === previousBlock.tag)
              .findIndex(block => block.ordinal === previousBlock.ordinal);
          }
        } catch {
          previousBlock = undefined;
        }
        if (previousBlock) break;
      }
      if (previousBlock) break;
    }
    if (!previousBlock) {
      throw new Error(`cannot locate stale block role override source: ${rule.document}#${rule.sha256}`);
    }
    const candidates = currentBlocks.filter(block =>
      block.heading === previousBlock.heading && block.tag === previousBlock.tag);
    const rankedCandidates = candidates
      .map(candidate => ({candidate, score: blockSimilarity(previousBlock.source, candidate.source)}))
      .sort((left, right) => right.score - left.score);
    const candidate = candidates.length === 1
      ? candidates[0]
      : rankedCandidates[0]?.score > rankedCandidates[1]?.score
        ? rankedCandidates[0].candidate
        : candidates[previousPeerIndex];
    if (!candidate) {
      throw new Error(`block role override refresh ambiguity count=${candidates.length}: ${rule.document}#${rule.sha256}`);
    }
    rule.sha256 = sha256(candidate.source);
  }
};

const reviewedIdentitySet = (label, review, identities) => {
  const sorted = uniqueSorted(identities);
  const digest = sha256(stableJson(sorted));
  const expectedCount = review?.count;
  const expectedDigest = review?.sha256;
  const matched = Number.isInteger(expectedCount)
    && expectedCount === sorted.length
    && typeof expectedDigest === 'string'
    && expectedDigest === digest
    && typeof review.specReason === 'string'
    && review.specReason.trim().length > 0;
  return {
    count: sorted.length,
    digest,
    identities: sorted,
    matched,
    message: `${label} review differs: expected_count=${expectedCount ?? 'missing'}`
      + ` actual_count=${sorted.length} expected_sha256=${expectedDigest || 'missing'}`
      + ` actual_sha256=${digest}`,
  };
};

const consolidateBaselineMembers = members => {
  const consolidated = new Map();
  for (const member of members) {
    const existing = consolidated.get(member.canonicalIdentity);
    if (!existing) {
      consolidated.set(member.canonicalIdentity, {
        ...member,
        baselineSources: [member.exactInterface],
        baselineRepresentations: [member.identity],
      });
      continue;
    }
    if (existing.canonicalSignatureSha256 !== member.canonicalSignatureSha256) {
      throw new Error(`baseline canonical member identity has conflicting signatures: ${member.canonicalIdentity}`);
    }
    existing.baselineSources = uniqueSorted([...existing.baselineSources, member.exactInterface]);
    existing.baselineRepresentations = uniqueSorted([
      ...existing.baselineRepresentations, member.identity,
    ]);
    existing.categoryIds = uniqueSorted([...existing.categoryIds, ...member.categoryIds]);
  }
  return [...consolidated.values()]
    .sort((left, right) => left.identity.localeCompare(right.identity, 'en'));
};

const consolidateTargetMembers = members => {
  const consolidated = new Map();
  for (const member of members) {
    const key = member.canonicalIdentity;
    const existing = consolidated.get(key);
    if (!existing) {
      consolidated.set(key, {
        ...member,
        representationSources: [{
          codeBlockId: member.codeBlockId,
          exactInterface: member.exactInterface,
          identity: member.identity,
          signatureSha256: member.signatureSha256,
        }],
      });
      continue;
    }
    if (existing.canonicalSignatureSha256 !== member.canonicalSignatureSha256) {
      throw new Error(`canonical member identity has conflicting signatures: ${key}`);
    }
    existing.categoryIds = uniqueSorted([...existing.categoryIds, ...member.categoryIds]);
    existing.representationSources.push({
      codeBlockId: member.codeBlockId,
      exactInterface: member.exactInterface,
      identity: member.identity,
      signatureSha256: member.signatureSha256,
    });
    const existingIsSource = existing.language === 'java' && existing.ownerIdentity.includes('::');
    const memberIsSource = member.language === 'java' && member.ownerIdentity.includes('::');
    if (!existingIsSource && memberIsSource) {
      const sources = existing.representationSources;
      Object.assign(existing, member);
      existing.representationSources = sources;
    }
  }
  for (const member of consolidated.values()) {
    member.representationSources = member.representationSources
      .sort((left, right) => left.identity.localeCompare(right.identity, 'en'));
  }
  return [...consolidated.values()]
    .sort((left, right) => left.identity.localeCompare(right.identity, 'en'));
};

const consolidateTargetOwners = owners => {
  const consolidated = new Map();
  for (const owner of owners) {
    const existing = consolidated.get(owner.identity);
    if (!existing) {
      consolidated.set(owner.identity, {
        ...owner,
        representationSources: [{
          codeBlockIds: owner.codeBlockIds,
          exactInterface: owner.exactInterface,
        }],
      });
      continue;
    }
    existing.categoryIds = uniqueSorted([...existing.categoryIds, ...owner.categoryIds]);
    existing.codeBlockIds = uniqueSorted([...existing.codeBlockIds, ...owner.codeBlockIds]);
    existing.representationSources.push({
      codeBlockIds: owner.codeBlockIds,
      exactInterface: owner.exactInterface,
    });
  }
  return [...consolidated.values()]
    .sort((left, right) => left.identity.localeCompare(right.identity, 'en'));
};

const buildTrace = (config, {refreshReview = false} = {}) => {
  if (refreshReview) refreshBlockRoleOverrides(config);
  const decisionIds = new Set(config.decisions.map(decision => decision.id));
  const categoryIds = new Set(config.categories.map(category => category.id));
  const baselineRevision = config.baseline?.revision;
  if (!/^[0-9a-f]{40}$/u.test(baselineRevision || '')) {
    throw new Error('baseline.revision must be one reviewed full Git commit ID');
  }
  gitText(['cat-file', '-e', `${baselineRevision}^{commit}`]);
  const forbiddenExactCodePatterns = (config.forbiddenExactCodePatterns || [])
    .map(expression => ({
      expression,
      pattern: compilePattern(expression, 'forbiddenExactCodePatterns'),
    }));
  const documents = [];
  const codeBlocks = [];
  let declarationOwners = [];
  let members = [];
  const targetOwnerIdentities = new Set();
  const ownershipProfiles = new Map();
  const profileFor = (categoryIdsForProfile, languageConfig, decisionOverride = undefined) => {
    const ownership = ownershipFor(config, categoryIdsForProfile, languageConfig);
    if (decisionOverride) ownership.decisionIds = uniqueSorted(decisionOverride);
    const body = {
      categoryIds: uniqueSorted(categoryIdsForProfile),
      language: languageConfig.id,
      packages: languageConfig.defaultPackages,
      ...ownership,
    };
    const id = `ownership:${sha256(stableJson(body)).slice(0, 20)}`;
    if (!ownershipProfiles.has(id)) ownershipProfiles.set(id, {id, ...body});
    return ownershipProfiles.get(id);
  };

  for (const languageConfig of config.languages) {
    const language = languageConfig.id;
    const exactDocuments = exactDocumentPaths(repositoryRoot, language);
    for (const document of exactDocuments) {
      const source = fs.readFileSync(path.join(repositoryRoot, document), 'utf8');
      const categories = config.categories
        .filter(category => category.documentPatterns.some(expression =>
          compilePattern(expression, `${category.id} documentPatterns`).test(document)))
        .map(category => category.id);
      const packages = packagesForDocument(languageConfig, document);
      const ownershipProfile = profileFor(categories, {
        ...languageConfig,
        defaultPackages: packages,
      });
      const blocks = headedFencedBlocks(source);
      for (const block of blocks) {
        if (exactInterfaceBlockRole(config, languageConfig, document, block).role !== 'package-contract') continue;
        for (const forbidden of forbiddenExactCodePatterns) {
          if (forbidden.pattern.test(block.source)) {
            throw new Error(
              `${document}:${block.startLine} exposes forbidden Instance lifecycle data: ${forbidden.expression}`);
          }
        }
      }
      const blockRecords = blocks.map(block => {
        const idMaterial = `${document}\0${block.ordinal}\0${block.tag}\0${block.source}`;
        const id = `${language}:block:${sha256(idMaterial).slice(0, 20)}`;
        const role = exactInterfaceBlockRole(config, languageConfig, document, block);
        return {
          ...block,
          categoryIds: categories,
          document,
          id,
          language,
          ownershipProfileId: ownershipProfile.id,
          role: role.role,
          ...(role.reason ? {roleReason: role.reason} : {}),
          sha256: sha256(block.source),
        };
      });
      const documentRecord = {
        categoryIds: categories,
        codeBlockIds: blockRecords.map(block => block.id),
        exactInterface: document,
        language,
        ownershipProfileId: ownershipProfile.id,
        packages,
      };
      documents.push(documentRecord);
      codeBlocks.push(...blockRecords.map(({source: ignored, ...record}) => record));

      const ownerMap = new Map();
      for (const block of blockRecords) {
        const blockSource = blocks.find(candidate => candidate.ordinal === block.ordinal)?.source || '';
        if (block.role !== 'package-contract') continue;
        for (const ownerName of publicDeclarationNames(language, blockSource)) {
          if (!ownerMap.has(ownerName)) ownerMap.set(ownerName, []);
          ownerMap.get(ownerName).push(block.id);
        }
        members.push(...extractMembers({
          block: {...block, source: blockSource},
          categories,
          document,
          language,
          packages,
        }).map(member => ({
          ...member,
          ownershipProfileId: ownershipProfile.id,
        })));
      }
      for (const [ownerName, blockIds] of [...ownerMap].sort(([left], [right]) => left.localeCompare(right, 'en'))) {
        const ownerIdentity = qualifyOwner(language, packages, ownerName);
        targetOwnerIdentities.add(`${language}|${primaryPackage(packages)}|${ownerIdentity}`);
        declarationOwners.push({
          categoryIds: categories,
          codeBlockIds: uniqueSorted(blockIds),
          exactInterface: document,
          facets: ['package-export'],
          identity: `${language}|${primaryPackage(packages)}|${ownerIdentity}`,
          language,
          ownershipProfileId: ownershipProfile.id,
          ownerIdentity,
          ownerName,
          role: 'package-public',
        });
      }
    }
  }

  declarationOwners = consolidateTargetOwners(declarationOwners);
  for (const owner of declarationOwners) {
    const languageConfig = config.languages.find(candidate => candidate.id === owner.language);
    const packages = packagesForDocument(languageConfig, owner.exactInterface);
    owner.ownershipProfileId = profileFor(owner.categoryIds, {
      ...languageConfig,
      defaultPackages: packages,
    }).id;
  }
  members = consolidateTargetMembers(members);
  for (const member of members) {
    const languageConfig = config.languages.find(candidate => candidate.id === member.language);
    member.ownershipProfileId = profileFor(member.categoryIds, {
      ...languageConfig,
      defaultPackages: member.packages,
    }).id;
  }

  const baselineMembersRaw = [];
  const baselineOwnerIdentities = new Set();
  for (const languageConfig of config.languages) {
    const baselineDocuments = languageConfig.baselineDocuments || [];
    if (baselineDocuments.length === 0) {
      throw new Error(`${languageConfig.id} has no reviewed baselineDocuments`);
    }
    for (const baselineDocument of baselineDocuments) {
      const document = baselineDocument.path;
      const categories = uniqueSorted(baselineDocument.categoryIds || []);
      const packages = baselineDocument.packages || languageConfig.defaultPackages;
      if (!document || categories.length === 0
          || !categories.every(categoryId => categoryIds.has(categoryId))) {
        throw new Error(`${languageConfig.id} baseline document has invalid ownership: ${document || '<missing>'}`);
      }
      const source = gitText([
        'show',
        `${baselineRevision}:${historicalSpecDocument(document)}`,
      ]);
      const blocks = headedFencedBlocks(source, languageConfig.acceptedCodeTags);
      for (const block of blocks) {
        const idMaterial = `${baselineRevision}\0${document}\0${block.ordinal}\0${block.tag}\0${block.source}`;
        const baselineBlock = {
          ...block,
          id: `${languageConfig.id}:baseline-block:${sha256(idMaterial).slice(0, 20)}`,
        };
        for (const ownerName of publicDeclarationNames(languageConfig.id, block.source)) {
          const ownerIdentity = qualifyOwner(languageConfig.id, packages, ownerName);
          baselineOwnerIdentities.add(
            `${languageConfig.id}|${primaryPackage(packages)}|${ownerIdentity}`);
        }
        baselineMembersRaw.push(...extractMembers({
          block: baselineBlock,
          categories,
          disposition: 'baseline-candidate',
          document,
          language: languageConfig.id,
          packages,
        }));
      }
    }
    for (const baselineSource of languageConfig.baselineSources || []) {
      const document = baselineSource.path;
      const categories = uniqueSorted(baselineSource.categoryIds || []);
      const packages = baselineSource.packages || languageConfig.defaultPackages;
      const tag = baselineSource.codeTag;
      if (!document || categories.length === 0
          || !categories.every(categoryId => categoryIds.has(categoryId))
          || !languageConfig.acceptedCodeTags.includes(tag)
          || baselineSource.publicSourceOnly !== true) {
        throw new Error(`${languageConfig.id} baseline source has invalid ownership or parser policy: ${document || '<missing>'}`);
      }
      const source = gitText(['show', `${baselineRevision}:${document}`])
        .replace(/[ \t]+$/gmu, '')
        .trim();
      const idMaterial = `${baselineRevision}\0${document}\0source\0${tag}\0${source}`;
      const baselineBlock = {
        heading: 'frozen public source',
        ordinal: 0,
        source,
        startLine: 1,
        tag,
        id: `${languageConfig.id}:baseline-source:${sha256(idMaterial).slice(0, 20)}`,
      };
      for (const ownerName of publicDeclarationNames(languageConfig.id, source)) {
        const ownerIdentity = qualifyOwner(languageConfig.id, packages, ownerName);
        baselineOwnerIdentities.add(
          `${languageConfig.id}|${primaryPackage(packages)}|${ownerIdentity}`);
      }
      baselineMembersRaw.push(...extractMembers({
        block: baselineBlock,
        categories,
        disposition: 'baseline-candidate',
        document,
        language: languageConfig.id,
        packages,
        publicSourceOnly: true,
      }));
    }
  }

  const baselineMembers = consolidateBaselineMembers(baselineMembersRaw);
  const baselineByIdentity = new Map(baselineMembers.map(member => [member.identity, member]));
  const baselineByCanonicalIdentity = new Map(
    baselineMembers.map(member => [member.canonicalIdentity, member]));
  const targetByIdentity = new Map(members.map(member => [member.identity, member]));
  if (targetByIdentity.size !== members.length) {
    throw new Error('target exact-interface parser produced duplicate member identities');
  }
  const consumedBaseline = new Set();
  const classifiedTarget = new Set();
  let exactSignatureMatches = 0;
  let overrideMatches = 0;
  for (const member of members) {
    const baseline = baselineByIdentity.get(member.identity);
    if (!baseline || baseline.signatureSha256 !== member.signatureSha256) continue;
    member.disposition = 'verified-baseline';
    member.dispositionEvidence = 'exact-baseline-signature';
    member.baselineIdentity = baseline.identity;
    member.baselineSignatureSha256 = baseline.signatureSha256;
    classifiedTarget.add(member.identity);
    consumedBaseline.add(baseline.identity);
    exactSignatureMatches += 1;
  }
  let canonicalSignatureMatches = 0;
  for (const member of members) {
    if (classifiedTarget.has(member.identity)) continue;
    const baseline = baselineByCanonicalIdentity.get(member.canonicalIdentity);
    if (!baseline || consumedBaseline.has(baseline.identity)
        || baseline.canonicalSignatureSha256 !== member.canonicalSignatureSha256) continue;
    member.disposition = 'verified-baseline';
    member.dispositionEvidence = 'canonical-baseline-signature';
    member.baselineIdentity = baseline.identity;
    member.baselineCanonicalIdentity = baseline.canonicalIdentity;
    member.baselineSignatureSha256 = baseline.signatureSha256;
    classifiedTarget.add(member.identity);
    consumedBaseline.add(baseline.identity);
    canonicalSignatureMatches += 1;
  }

  const overrideBaselineIdentities = new Set();
  const overrideTargetIdentities = new Set();
  let supersededOverrides = 0;
  if (refreshReview) {
    const refreshedOverrides = [];
    const refreshedBaselineIdentities = new Set();
    const refreshedTargetIdentities = new Set();
    const orderedOverrides = orderOverridesForRefresh(
      config.baseline?.memberOverrides || [],
      baselineByIdentity,
      targetByIdentity);
    for (const override of orderedOverrides) {
      let baseline = baselineByIdentity.get(override.baselineIdentity);
      if (!baseline || baseline.signatureSha256 !== override.baselineSignatureSha256) {
        const identityPrefix = override.baselineIdentity?.slice(
          0,
          override.baselineIdentity.lastIndexOf('#') + 1);
        const candidates = baselineMembers.filter(member =>
          identityPrefix && member.identity.startsWith(identityPrefix));
        if (candidates.length !== 1) {
          throw new Error(`member override does not name an exact baseline signature: ${override.baselineIdentity || '<missing>'}`);
        }
        [baseline] = candidates;
      }
      let target = targetByIdentity.get(override.targetIdentity);
      if (!target || target.signatureSha256 !== override.targetSignatureSha256) {
        const candidates = members.filter(member =>
          memberOwnerNameKey(member) === memberOwnerNameKey(baseline)
          && !classifiedTarget.has(member.identity)
          && !refreshedTargetIdentities.has(member.identity));
        if (candidates.length === 0) continue;
        if (candidates.length !== 1) {
          const error = new Error(`override refresh ambiguity count=${candidates.length}: ${baseline.identity}`);
          error.code = 'TRACE_MEMBER_AMBIGUITY';
          error.candidates = [{
            baselineIdentity: baseline.identity,
            baselineNormalizedSignature: baseline.normalizedSignature,
            targetCandidates: candidates.map(candidate => ({
              targetIdentity: candidate.identity,
              targetSignatureSha256: candidate.signatureSha256,
              targetNormalizedSignature: candidate.normalizedSignature,
            })),
          }];
          throw error;
        }
        [target] = candidates;
      }
      if (classifiedTarget.has(target.identity)
          || refreshedBaselineIdentities.has(baseline.identity)
          || refreshedTargetIdentities.has(target.identity)) continue;
      refreshedOverrides.push({
        ...override,
        baselineIdentity: baseline.identity,
        baselineSignatureSha256: baseline.signatureSha256,
        targetIdentity: target.identity,
        targetSignatureSha256: target.signatureSha256,
      });
      refreshedBaselineIdentities.add(baseline.identity);
      refreshedTargetIdentities.add(target.identity);
    }
    config.baseline.memberOverrides = refreshedOverrides;
  }

  for (const override of config.baseline?.memberOverrides || []) {
    const baseline = baselineByIdentity.get(override.baselineIdentity);
    const target = targetByIdentity.get(override.targetIdentity);
    if (!baseline || !target
        || baseline.signatureSha256 !== override.baselineSignatureSha256
        || target.signatureSha256 !== override.targetSignatureSha256) {
      const targetCandidates = baseline
        ? members.filter(member => memberOwnerNameKey(member) === memberOwnerNameKey(baseline))
          .map(member => `${member.identity} (${member.signatureSha256}) ${member.normalizedSignature}`)
        : [];
      throw new Error(
        `member override does not name exact baseline and target signatures: ${override.baselineIdentity || '<missing>'}`
        + ` (baseline=${baseline?.signatureSha256 || '<missing>'}, target=${target?.signatureSha256 || '<missing>'})`
        + (targetCandidates.length > 0
          ? `; target candidates: ${targetCandidates.join(' | ')}`
          : '; target candidates: none'),
      );
    }
    if (classifiedTarget.has(target.identity)) {
      if (target.disposition !== 'verified-baseline') {
        throw new Error(`classified override target is not verified baseline: ${target.identity}`);
      }
      supersededOverrides += 1;
      continue;
    }
    const overrideReason = config.baseline?.overrideReasons?.[override.specReason];
    if (override.disposition !== 'v11-first-implementation'
        || typeof overrideReason !== 'string' || overrideReason.trim().length === 0) {
      throw new Error(`member override has no reviewed v11 disposition and spec reason: ${override.baselineIdentity}`);
    }
    if (consumedBaseline.has(baseline.identity) || classifiedTarget.has(target.identity)
        || overrideBaselineIdentities.has(baseline.identity)
        || overrideTargetIdentities.has(target.identity)) {
      throw new Error(`member override is ambiguous or reused: ${baseline.identity} -> ${target.identity}`);
    }
    target.disposition = override.disposition;
    target.dispositionEvidence = 'exact-identity-override';
    target.baselineIdentity = baseline.identity;
    target.baselineSignatureSha256 = baseline.signatureSha256;
    target.specReason = overrideReason;
    target.specReasonId = override.specReason;
    classifiedTarget.add(target.identity);
    consumedBaseline.add(baseline.identity);
    overrideBaselineIdentities.add(baseline.identity);
    overrideTargetIdentities.add(target.identity);
    overrideMatches += 1;
  }

  const remainingBaselineByName = new Map();
  for (const baseline of baselineMembers) {
    if (consumedBaseline.has(baseline.identity)) continue;
    const key = memberOwnerNameKey(baseline);
    if (!remainingBaselineByName.has(key)) remainingBaselineByName.set(key, []);
    remainingBaselineByName.get(key).push(baseline);
  }
  const ambiguousMembers = [];
  const newMembers = [];
  const remainingTargetsByName = new Map();
  for (const member of members) {
    if (classifiedTarget.has(member.identity)) continue;
    const key = memberOwnerNameKey(member);
    if (!remainingTargetsByName.has(key)) remainingTargetsByName.set(key, []);
    remainingTargetsByName.get(key).push(member);
  }
  for (const member of members) {
    if (classifiedTarget.has(member.identity)) continue;
    const baselineCandidates = remainingBaselineByName.get(memberOwnerNameKey(member)) || [];
    if (baselineCandidates.length > 0) {
      if (refreshReview && baselineCandidates.length === 1
          && remainingTargetsByName.get(memberOwnerNameKey(member))?.length === 1) {
        const [baseline] = baselineCandidates;
        const specReason = 'V11-SIGNATURE-CHANGE';
        const overrideReason = config.baseline?.overrideReasons?.[specReason];
        if (typeof overrideReason !== 'string' || overrideReason.trim().length === 0) {
          throw new Error(`missing reviewed override reason: ${specReason}`);
        }
        member.disposition = 'v11-first-implementation';
        member.dispositionEvidence = 'exact-identity-override';
        member.baselineIdentity = baseline.identity;
        member.baselineSignatureSha256 = baseline.signatureSha256;
        member.specReason = overrideReason;
        member.specReasonId = specReason;
        classifiedTarget.add(member.identity);
        consumedBaseline.add(baseline.identity);
        config.baseline.memberOverrides.push({
          baselineIdentity: baseline.identity,
          baselineSignatureSha256: baseline.signatureSha256,
          targetIdentity: member.identity,
          targetSignatureSha256: member.signatureSha256,
          disposition: 'v11-first-implementation',
          specReason,
        });
        overrideMatches += 1;
        continue;
      }
      ambiguousMembers.push({
        targetIdentity: member.identity,
        targetSignatureSha256: member.signatureSha256,
        targetNormalizedSignature: member.normalizedSignature,
        baselineCandidates: baselineCandidates.map(candidate => ({
          baselineIdentity: candidate.identity,
          baselineSignatureSha256: candidate.signatureSha256,
          baselineNormalizedSignature: candidate.normalizedSignature,
        })),
      });
    } else {
      newMembers.push(member);
    }
  }
  if (ambiguousMembers.length > 0) {
    const error = new Error(
      `unreviewed same-owner/member baseline ambiguity count=${ambiguousMembers.length}`);
    error.code = 'TRACE_MEMBER_AMBIGUITY';
    error.candidates = ambiguousMembers;
    throw error;
  }

  const newMemberReview = reviewedIdentitySet(
    'reviewed v11-first member set',
    config.baseline?.reviewedV11FirstMembers,
    newMembers.map(member => `${member.identity}#${member.signatureSha256}`));
  if (refreshReview) {
    config.baseline.reviewedV11FirstMembers.count = newMemberReview.count;
    config.baseline.reviewedV11FirstMembers.sha256 = newMemberReview.digest;
  } else if (!newMemberReview.matched) {
    throw new Error(`${newMemberReview.message}\n`
      + newMemberReview.identities.slice(0, 50).join('\n'));
  }
  for (const member of newMembers) {
    member.disposition = 'v11-first-implementation';
    member.dispositionEvidence = `reviewed-new-member-set:${newMemberReview.digest}`;
    member.specReason = config.baseline.reviewedV11FirstMembers.specReason;
    classifiedTarget.add(member.identity);
  }

  const removalCandidates = baselineMembers
    .filter(member => !consumedBaseline.has(member.identity));
  const removalReview = reviewedIdentitySet(
    'reviewed intentional-removal member set',
    config.baseline?.reviewedIntentionalRemovals,
    removalCandidates.map(member => `${member.identity}#${member.signatureSha256}`));
  if (refreshReview) {
    config.baseline.reviewedIntentionalRemovals.count = removalReview.count;
    config.baseline.reviewedIntentionalRemovals.sha256 = removalReview.digest;
  } else if (!removalReview.matched) {
    throw new Error(`${removalReview.message}\n`
      + removalReview.identities.slice(0, 50).join('\n'));
  }

  const intentionalRemovals = removalCandidates.map(baseline => {
    const languageConfig = config.languages.find(candidate => candidate.id === baseline.language);
    const ownershipProfile = profileFor(
      baseline.categoryIds,
      {...languageConfig, defaultPackages: baseline.packages});
    const exactInterfaces = documents
      .filter(document => document.language === baseline.language
        && document.categoryIds.some(categoryId => baseline.categoryIds.includes(categoryId)))
      .map(document => document.exactInterface);
    return {
      baselineExactInterfaces: baseline.baselineSources,
      baselineSignatureSha256: baseline.signatureSha256,
      categoryIds: baseline.categoryIds,
      disposition: 'intentional-removal',
      exactInterfaces,
      identity: baseline.identity,
      kind: baseline.kind,
      language: baseline.language,
      memberName: baseline.memberName,
      ownerIdentity: baseline.ownerIdentity,
      ownershipProfileId: ownershipProfile.id,
      removalId: 'REVIEWED-BASELINE-REMOVAL',
      specReason: config.baseline.reviewedIntentionalRemovals.specReason,
    };
  });

  const newOwnerReview = reviewedIdentitySet(
    'reviewed v11-first declaration-owner set',
    config.baseline?.reviewedV11FirstOwners,
    [...targetOwnerIdentities].filter(identity => !baselineOwnerIdentities.has(identity)));
  if (refreshReview) {
    config.baseline.reviewedV11FirstOwners.count = newOwnerReview.count;
    config.baseline.reviewedV11FirstOwners.sha256 = newOwnerReview.digest;
  } else if (!newOwnerReview.matched) throw new Error(newOwnerReview.message);
  for (const owner of declarationOwners) {
    if (baselineOwnerIdentities.has(owner.identity)) {
      owner.disposition = 'verified-baseline';
      owner.dispositionEvidence = 'exact-baseline-owner';
    } else {
      owner.disposition = 'v11-first-implementation';
      owner.dispositionEvidence = `reviewed-new-owner-set:${newOwnerReview.digest}`;
    }
  }
  for (const member of members) delete member.normalizedSignature;

  const memberOverloads = new Map();
  for (const member of members) {
    const key = `${member.language}\0${member.ownerIdentity}\0${member.memberName}`;
    if (!memberOverloads.has(key)) memberOverloads.set(key, []);
    memberOverloads.get(key).push(member);
  }
  for (const group of memberOverloads.values()) {
    if (new Set(group.map(member => member.signatureSha256)).size > 1) {
      for (const member of group) member.facets = uniqueSorted([...member.facets, 'overload']);
    }
  }

  const dispositions = [...declarationOwners, ...members,
    ...intentionalRemovals].reduce((counts, record) => {
    counts[record.disposition] = (counts[record.disposition] || 0) + 1;
    return counts;
  }, Object.create(null));
  const facets = [...declarationOwners, ...members].flatMap(record => record.facets)
    .reduce((counts, facet) => {
      counts[facet] = (counts[facet] ?? 0) + 1;
      return counts;
    }, Object.create(null));
  const codeBlockRoles = codeBlocks.reduce((counts, block) => {
    counts[block.role] = (counts[block.role] || 0) + 1;
    return counts;
  }, Object.create(null));

  for (const category of config.categories) {
    if (!category.decisionIds.every(id => decisionIds.has(id))) {
      throw new Error(`${category.id} refers to an unknown POSD decision`);
    }
  }
  const referencedDecisions = new Set(config.categories.flatMap(category => category.decisionIds));
  const unreferencedDecisions = config.decisions
    .map(decision => decision.id)
    .filter(id => !referencedDecisions.has(id));
  if (unreferencedDecisions.length > 0) {
    throw new Error(`POSD decisions have no contract category: ${unreferencedDecisions.join(', ')}`);
  }
  const scenarioIds = config.categories.flatMap(category => category.e2eScenarioIds);
  const scenarioCounts = scenarioIds.reduce((counts, id) => {
    counts.set(id, (counts.get(id) || 0) + 1);
    return counts;
  }, new Map());
  const requiredScenarios = requiredScenarioIds();
  const missingScenarios = requiredScenarios.filter(id => !scenarioCounts.has(id));
  const duplicateScenarios = [...scenarioCounts]
    .filter(([, count]) => count !== 1)
    .map(([id]) => id);
  const unknownScenarios = [...scenarioCounts.keys()]
    .filter(id => !requiredScenarios.includes(id));
  if (missingScenarios.length || duplicateScenarios.length || unknownScenarios.length) {
    throw new Error(`scenario ownership differs: missing=${missingScenarios.join(',') || 'none'}`
      + ` duplicate=${duplicateScenarios.join(',') || 'none'}`
      + ` unknown=${unknownScenarios.join(',') || 'none'}`);
  }
  const overrideKeys = new Set(config.blockRoleOverrides?.map(rule => `${rule.document}\0${rule.sha256}`));
  const observedOverrideKeys = new Set(codeBlocks
    .filter(block => overrideKeys.has(`${block.document}\0${block.sha256}`))
    .map(block => `${block.document}\0${block.sha256}`));
  if (overrideKeys.size !== (config.blockRoleOverrides || []).length
      || observedOverrideKeys.size !== overrideKeys.size) {
    throw new Error('block role overrides are duplicate or stale');
  }
  for (const removal of config.removalPolicies || []) {
    if (!removal.categoryIds.every(id => categoryIds.has(id))) {
      throw new Error(`${removal.id} refers to an unknown category`);
    }
  }

  return {
    schema: 1,
    version: '11.0.0',
    sourcePolicy: 'Every exact-interface fence has a deterministic role. Only package-contract blocks own target signatures. Member dispositions come only from an exact or canonical reviewed Git baseline match, an exact-identity override, or a reviewed exact-set commitment.',
    config: path.relative(repositoryRoot, configPath).split(path.sep).join('/'),
    configSha256: sha256(stableJson(config)),
    summary: {
      categories: config.categories.length,
      ambiguousMembers: 0,
      baselineDuplicateSources: baselineMembersRaw.length - baselineMembers.length,
      baselineMembers: baselineMembers.length,
      baselineRevision,
      codeBlocks: codeBlocks.length,
      codeBlockRoles: Object.fromEntries(Object.entries(codeBlockRoles).sort()),
      declarationOwners: declarationOwners.length,
      decisions: config.decisions.length,
      dispositions: Object.fromEntries(Object.entries(dispositions).sort()),
      documents: documents.length,
      facets: Object.fromEntries(Object.entries(facets).sort()),
      forbiddenExactCodeMatches: 0,
      exactSignatureMatches,
      canonicalSignatureMatches,
      intentionalRemovals: intentionalRemovals.length,
      languages: config.languages.length,
      members: members.length,
      overrideMatches,
      ownershipProfiles: ownershipProfiles.size,
      reviewedNewMembers: newMembers.length,
      reviewedNewOwners: newOwnerReview.count,
      supersededOverrides,
      unclassifiedMembers: members.length - classifiedTarget.size,
      unknownOrUnowned: 0,
    },
    baseline: {
      revision: baselineRevision,
      documentCount: config.languages.reduce(
        (count, language) => count + (language.baselineDocuments || []).length, 0),
      sourceCount: config.languages.reduce(
        (count, language) => count + (language.baselineSources || []).length, 0),
      memberCount: baselineMembers.length,
      reviewedIntentionalRemovalSetSha256: removalReview.digest,
      reviewedV11FirstMemberSetSha256: newMemberReview.digest,
      reviewedV11FirstOwnerSetSha256: newOwnerReview.digest,
    },
    categories: config.categories.map(category => ({
      id: category.id,
      decisionIds: category.decisionIds,
      e2eScenarioIds: category.e2eScenarioIds,
      formalSpecOwners: category.formalSpecOwners,
    })),
    decisions: config.decisions,
    opaqueProviderIdentifierMappings: config.opaqueProviderIdentifierMappings,
    removalPolicies: config.removalPolicies || [],
    ownershipProfiles: [...ownershipProfiles.values()]
      .sort((left, right) => left.id.localeCompare(right.id, 'en')),
    documents: documents.sort((left, right) => left.exactInterface.localeCompare(right.exactInterface, 'en')),
    codeBlocks: codeBlocks.sort((left, right) => left.id.localeCompare(right.id, 'en')),
    declarationOwners: declarationOwners.sort((left, right) => left.identity.localeCompare(right.identity, 'en')),
    members: members.sort((left, right) => left.identity.localeCompare(right.identity, 'en')),
    intentionalRemovals: intentionalRemovals.sort((left, right) => left.identity.localeCompare(right.identity, 'en')),
  };
};

const validationFailures = trace => {
  const failures = [];
  const fail = message => failures.push(message);
  const requiredOwnership = [
    'contractTestOwners',
    'decisionIds',
    'e2eScenarioIds',
    'formalSpecOwners',
    'implementationLedgerIds',
    'packageGateIds',
    'packages',
  ];
  if (trace.schema !== 1 || trace.version !== '11.0.0') fail('trace schema or version differs');
  if (!/^[0-9a-f]{40}$/u.test(trace.baseline?.revision || '')
      || trace.summary?.baselineRevision !== trace.baseline?.revision) {
    fail('trace has no fixed reviewed baseline revision');
  }
  const decisions = new Map((trace.decisions || []).map(decision => [decision.id, decision]));
  const categories = new Map((trace.categories || []).map(category => [category.id, category]));
  const ownershipProfiles = new Map(
    (trace.ownershipProfiles || []).map(profile => [profile.id, profile]));
  const documents = new Map((trace.documents || []).map(document => [document.exactInterface, document]));
  const blocks = new Map((trace.codeBlocks || []).map(block => [block.id, block]));
  const opaqueMappings = trace.opaqueProviderIdentifierMappings || [];
  const mappingLanguages = opaqueMappings.map(mapping => mapping.language);
  if (stableJson(uniqueSorted(mappingLanguages)) !== stableJson(['cpp', 'dotnet', 'java', 'kotlin', 'node'])
      || new Set(mappingLanguages).size !== mappingLanguages.length) {
    fail('opaque provider identifier mappings do not cover each public language exactly once');
  }
  for (const mapping of opaqueMappings) {
    for (const key of ['authorityKey', 'storeVersion', 'checkpointReference', 'scanCursor', 'policy']) {
      if (typeof mapping[key] !== 'string' || mapping[key].trim().length === 0) {
        fail(`opaque provider identifier mapping is incomplete: ${mapping.language}: ${key}`);
      }
    }
  }
  const canonicalMemberIdentities = (trace.members || []).map(member => member.canonicalIdentity);
  if (new Set(canonicalMemberIdentities).size !== canonicalMemberIdentities.length) {
    fail('canonical member identity is not unique');
  }
  for (const [label, records, key] of [
    ['document', trace.documents || [], record => record.exactInterface],
    ['code block', trace.codeBlocks || [], record => record.id],
    ['declaration owner', trace.declarationOwners || [], record => record.identity],
    ['member', trace.members || [], record => record.identity],
    ['intentional removal', trace.intentionalRemovals || [], record => record.identity],
    ['ownership profile', trace.ownershipProfiles || [], record => record.id],
  ]) {
    const identities = records.map(key);
    if (new Set(identities).size !== identities.length) fail(`${label} identity is not unique`);
  }
  for (const decision of decisions.values()) {
    if (!Array.isArray(decision.alternatives) || decision.alternatives.length < 2
        || !decision.selected || !decision.callerBurden || !decision.informationHiding) {
      fail(`POSD decision is incomplete: ${decision.id}`);
    }
  }
  const referencedDecisions = new Set(
    [...categories.values()].flatMap(category => category.decisionIds || []));
  for (const decisionId of decisions.keys()) {
    if (!referencedDecisions.has(decisionId)) {
      fail(`POSD decision has no contract category: ${decisionId}`);
    }
  }
  const scenarioIds = [...categories.values()].flatMap(category => category.e2eScenarioIds || []);
  const scenarioCounts = scenarioIds.reduce((counts, id) => {
    counts.set(id, (counts.get(id) || 0) + 1);
    return counts;
  }, new Map());
  const expectedScenarios = requiredScenarioIds();
  for (const id of expectedScenarios) {
    if (!scenarioCounts.has(id)) fail(`required E2E scenario is missing: ${id}`);
  }
  for (const [id, count] of scenarioCounts) {
    if (!expectedScenarios.includes(id)) fail(`unknown E2E scenario: ${id}`);
    if (count !== 1) fail(`E2E scenario has duplicate category ownership: ${id}`);
  }
  const validateOwnership = (kind, record, identity) => {
    if (!Array.isArray(record.categoryIds) || record.categoryIds.length === 0) {
      fail(`${kind} is unowned: ${identity}: categoryIds`);
      return;
    }
    const profile = ownershipProfiles.get(record.ownershipProfileId);
    if (!profile) {
      fail(`${kind} is unowned: ${identity}: ownershipProfileId`);
      return;
    }
    if (JSON.stringify(uniqueSorted(record.categoryIds)) !== JSON.stringify(profile.categoryIds)) {
      fail(`${kind} category/profile mismatch: ${identity}`);
    }
    for (const key of requiredOwnership) {
      if (!Array.isArray(profile[key]) || profile[key].length === 0) {
        fail(`${kind} is unowned: ${identity}: ${key}`);
      }
    }
    for (const categoryId of record.categoryIds || []) {
      if (!categories.has(categoryId)) fail(`${kind} uses unknown category ${categoryId}: ${identity}`);
    }
    for (const decisionId of profile.decisionIds || []) {
      if (!decisions.has(decisionId)) fail(`${kind} uses unknown POSD decision ${decisionId}: ${identity}`);
    }
    for (const ownerPath of profile.formalSpecOwners || []) {
      if (!fs.existsSync(path.join(repositoryRoot, ownerPath))) {
        fail(`${kind} refers to a missing spec owner ${ownerPath}: ${identity}`);
      }
    }
  };
  for (const document of trace.documents || []) {
    validateOwnership('document', document, document.exactInterface);
    if (!fs.existsSync(path.join(repositoryRoot, document.exactInterface))) {
      fail(`exact document is missing: ${document.exactInterface}`);
    }
    for (const blockId of document.codeBlockIds || []) {
      if (!blocks.has(blockId)) fail(`document refers to a missing code block: ${document.exactInterface}: ${blockId}`);
    }
  }
  for (const block of trace.codeBlocks || []) {
    validateOwnership('code block', block, block.id);
    if (!documents.has(block.document)) fail(`code block refers to a missing document: ${block.id}`);
    if (!/^[0-9a-f]{64}$/u.test(block.sha256 || '')) fail(`code block has no source hash: ${block.id}`);
    if (!['package-contract', 'application-example', 'documentation-support'].includes(block.role)) {
      fail(`code block has unknown role: ${block.id}`);
    }
    if (block.role !== 'package-contract' && !block.roleReason) {
      fail(`excluded code block has no deterministic reason: ${block.id}`);
    }
  }
  for (const owner of trace.declarationOwners || []) {
    validateOwnership('declaration owner', owner, owner.identity);
    if (!documents.has(owner.exactInterface)) fail(`declaration owner refers to a missing document: ${owner.identity}`);
    if (!Array.isArray(owner.codeBlockIds) || owner.codeBlockIds.length === 0
        || owner.codeBlockIds.some(id => !blocks.has(id))) {
      fail(`declaration owner has no exact code-block source: ${owner.identity}`);
    }
    if (owner.disposition === 'verified-baseline'
        && owner.dispositionEvidence !== 'exact-baseline-owner') {
      fail(`verified declaration owner lacks exact baseline evidence: ${owner.identity}`);
    }
    if (owner.disposition === 'v11-first-implementation'
        && !/^reviewed-new-owner-set:[0-9a-f]{64}$/u.test(owner.dispositionEvidence || '')) {
      fail(`new declaration owner lacks reviewed exact-set evidence: ${owner.identity}`);
    }
  }
  for (const member of trace.members || []) {
    validateOwnership('member', member, member.identity);
    if (!documents.has(member.exactInterface) || !blocks.has(member.codeBlockId)) {
      fail(`member has no exact source owner: ${member.identity}`);
    }
    if (!member.ownerIdentity || !member.memberName || !/^[0-9a-f]{64}$/u.test(member.signatureSha256 || '')) {
      fail(`member identity is incomplete: ${member.identity}`);
    }
    if (!member.canonicalIdentity || !/^[0-9a-f]{64}$/u.test(member.canonicalSignatureSha256 || '')) {
      fail(`member canonical identity is incomplete: ${member.identity}`);
    }
    if (!Array.isArray(member.representationSources) || member.representationSources.length === 0) {
      fail(`member has no source or binary representation evidence: ${member.identity}`);
    }
    if (member.disposition === 'verified-baseline') {
      const exact = member.dispositionEvidence === 'exact-baseline-signature'
        && member.baselineIdentity === member.identity
        && member.baselineSignatureSha256 === member.signatureSha256;
      const canonical = member.dispositionEvidence === 'canonical-baseline-signature'
        && member.baselineCanonicalIdentity === member.canonicalIdentity
        && /^[0-9a-f]{64}$/u.test(member.baselineSignatureSha256 || '');
      if (!exact && !canonical) {
        fail(`verified member lacks an exact baseline signature match: ${member.identity}`);
      }
    } else if (member.disposition === 'v11-first-implementation') {
      const reviewedNew = /^reviewed-new-member-set:[0-9a-f]{64}$/u.test(
        member.dispositionEvidence || '');
      const exactOverride = member.dispositionEvidence === 'exact-identity-override'
        && member.baselineIdentity && /^[0-9a-f]{64}$/u.test(member.baselineSignatureSha256 || '')
        && typeof member.specReason === 'string' && member.specReason.length > 0;
      if (!reviewedNew && !exactOverride) {
        fail(`v11-first member lacks reviewed exact identity evidence: ${member.identity}`);
      }
    }
  }
  for (const removal of trace.intentionalRemovals || []) {
    validateOwnership('intentional removal', removal, removal.identity);
    if (removal.disposition !== 'intentional-removal') {
      fail(`removed surface has an invalid disposition: ${removal.identity}`);
    }
    if (removal.identity.includes('<removed>')
        || !removal.ownerIdentity || !removal.memberName
        || !/^[0-9a-f]{64}$/u.test(removal.baselineSignatureSha256 || '')
        || !Array.isArray(removal.baselineExactInterfaces)
        || removal.baselineExactInterfaces.length === 0
        || typeof removal.specReason !== 'string' || removal.specReason.length === 0) {
      fail(`removed surface is not an exact baseline member identity: ${removal.identity}`);
    }
    if (!Array.isArray(removal.exactInterfaces) || removal.exactInterfaces.length === 0
        || removal.exactInterfaces.some(document => !documents.has(document))) {
      fail(`removed surface has no exact-interface absence owner: ${removal.identity}`);
    }
  }
  for (const category of categories.values()) {
    const categoryDocumentCount = (trace.documents || [])
      .filter(document => document.categoryIds.includes(category.id)).length;
    const categoryBlockCount = (trace.codeBlocks || [])
      .filter(block => block.categoryIds.includes(category.id)).length;
    const categoryOwnerCount = (trace.declarationOwners || [])
      .filter(owner => owner.categoryIds.includes(category.id)).length;
    if (categoryDocumentCount === 0 || categoryBlockCount === 0 || categoryOwnerCount === 0) {
      fail(`category is uncovered: ${category.id}: documents=${categoryDocumentCount} blocks=${categoryBlockCount} owners=${categoryOwnerCount}`);
    }
  }
  const requiredFacets = [
    'callback',
    'constructor',
    'decorator',
    'di-token',
    'enum-value',
    'extension',
    'generic-bound',
    'overload',
    'package-export',
  ];
  const observedFacets = new Set([
    ...(trace.declarationOwners || []).flatMap(owner => owner.facets || []),
    ...(trace.members || []).flatMap(member => member.facets || []),
  ]);
  for (const facet of requiredFacets) {
    if (!observedFacets.has(facet)) fail(`required public-contract facet is uncovered: ${facet}`);
  }
  const allowedDispositions = new Set([
    'verified-baseline',
    'v11-first-implementation',
    'intentional-removal',
  ]);
  for (const record of [
    ...(trace.declarationOwners || []),
    ...(trace.members || []),
    ...(trace.intentionalRemovals || []),
  ]) {
    if (!allowedDispositions.has(record.disposition)) {
      fail(`unknown disposition: ${record.identity || record.id || record.exactInterface}`);
    }
  }
  if ((trace.summary?.unknownOrUnowned ?? -1) !== 0) fail('trace summary reports unknown or unowned entries');
  if ((trace.summary?.unclassifiedMembers ?? -1) !== 0) {
    fail('trace summary reports unclassified members');
  }
  if ((trace.summary?.ambiguousMembers ?? -1) !== 0) {
    fail('trace summary reports ambiguous members');
  }
  const memberDispositionCounts = (trace.members || []).reduce((counts, member) => {
    counts[member.disposition] = (counts[member.disposition] || 0) + 1;
    return counts;
  }, Object.create(null));
  const exactCount = trace.summary?.exactSignatureMatches ?? -1;
  const canonicalCount = trace.summary?.canonicalSignatureMatches ?? -1;
  const overrideCount = trace.summary?.overrideMatches ?? -1;
  const newCount = trace.summary?.reviewedNewMembers ?? -1;
  if (exactCount + canonicalCount + overrideCount + newCount !== (trace.members?.length || 0)
      || memberDispositionCounts['verified-baseline'] !== exactCount + canonicalCount
      || memberDispositionCounts['v11-first-implementation'] !== overrideCount + newCount) {
    fail('trace summary member disposition counts differ from exact baseline evidence');
  }
  if ((trace.summary?.forbiddenExactCodeMatches ?? -1) !== 0) {
    fail('trace summary reports a forbidden Instance lifecycle public member');
  }
  for (const [key, actual] of [
    ['documents', trace.documents?.length || 0],
    ['codeBlocks', trace.codeBlocks?.length || 0],
    ['declarationOwners', trace.declarationOwners?.length || 0],
    ['members', trace.members?.length || 0],
    ['intentionalRemovals', trace.intentionalRemovals?.length || 0],
    ['ownershipProfiles', trace.ownershipProfiles?.length || 0],
  ]) {
    if (trace.summary?.[key] !== actual) fail(`trace summary count differs: ${key}`);
  }
  return failures;
};

const runNegativeSelfTests = trace => {
  const tests = [
    {
      label: 'uncovered exact document',
      mutate: candidate => { candidate.documents[0].categoryIds = []; },
      expected: /document is unowned/u,
    },
    {
      label: 'uncovered code block',
      mutate: candidate => { candidate.codeBlocks[0].categoryIds = []; },
      expected: /code block is unowned/u,
    },
    {
      label: 'uncovered declaration owner',
      mutate: candidate => { candidate.declarationOwners[0].categoryIds = []; },
      expected: /declaration owner is unowned/u,
    },
    {
      label: 'uncovered category',
      mutate: candidate => {
        candidate.categories.push({
          id: 'negative-uncovered-category',
          decisionIds: [candidate.decisions[0].id],
          formalSpecOwners: candidate.categories[0].formalSpecOwners,
        });
      },
      expected: /category is uncovered/u,
    },
    {
      label: 'unowned member implementation',
      mutate: candidate => { candidate.members[0].ownershipProfileId = ''; },
      expected: /member is unowned/u,
    },
    {
      label: 'unclassified member disposition',
      mutate: candidate => { candidate.members[0].disposition = 'unclassified'; },
      expected: /unknown disposition/u,
    },
    {
      label: 'ambiguous member summary',
      mutate: candidate => { candidate.summary.ambiguousMembers = 1; },
      expected: /ambiguous members/u,
    },
    {
      label: 'forged exact baseline evidence',
      mutate: candidate => {
        const member = candidate.members.find(value => value.disposition === 'verified-baseline');
        member.baselineIdentity = `${member.baselineIdentity}-forged`;
      },
      expected: /exact baseline signature match/u,
    },
    {
      label: 'missing constructor coverage',
      mutate: candidate => {
        for (const member of candidate.members) {
          member.facets = member.facets.filter(facet => facet !== 'constructor');
        }
      },
      expected: /facet is uncovered: constructor/u,
    },
    {
      label: 'removed surface without exact absence owner',
      mutate: candidate => { candidate.intentionalRemovals[0].exactInterfaces = []; },
      expected: /removed surface has no exact-interface absence owner/u,
    },
    {
      label: 'conceptual Cartesian removal row',
      mutate: candidate => { candidate.intentionalRemovals[0].identity = 'cpp|pkg|<removed>.concept'; },
      expected: /not an exact baseline member identity/u,
    },
    {
      label: 'missing required E2E scenario',
      mutate: candidate => { candidate.categories[0].e2eScenarioIds.shift(); },
      expected: /required E2E scenario is missing/u,
    },
    {
      label: 'duplicate E2E category ownership',
      mutate: candidate => {
        candidate.categories[1].e2eScenarioIds.push(candidate.categories[0].e2eScenarioIds[0]);
      },
      expected: /duplicate category ownership/u,
    },
    {
      label: 'unknown E2E scenario',
      mutate: candidate => { candidate.categories[0].e2eScenarioIds[0] = 'V11-E2E-M99'; },
      expected: /unknown E2E scenario/u,
    },
    {
      label: 'unreferenced POSD decision',
      mutate: candidate => {
        const id = candidate.decisions[0].id;
        for (const category of candidate.categories) {
          category.decisionIds = category.decisionIds.filter(value => value !== id);
        }
      },
      expected: /POSD decision has no contract category/u,
    },
  ];
  for (const test of tests) {
    const candidate = structuredClone(trace);
    test.mutate(candidate);
    const failures = validationFailures(candidate);
    if (!failures.some(failure => test.expected.test(failure))) {
      throw new Error(`negative self-test did not reject ${test.label}: ${failures.join('; ')}`);
    }
  }
  return tests.length;
};

const runExtractorSelfTests = () => {
  const csharp = [
    'public readonly record struct ScanExpired(string Cursor);',
    'public readonly record struct ZLinkRelocationCapacityFence(string Value) {',
    '    public string Value { get; } = Value;',
    '}',
  ].join('\n');
  const spans = ownerSpans(csharp, 'csharp');
  const scanExpired = spans.find(span => span.name === 'ScanExpired');
  if (!scanExpired
      || scanExpired.open !== -1
      || csharp.slice(scanExpired.start, scanExpired.close + 1)
        !== 'public readonly record struct ScanExpired(string Cursor);') {
    throw new Error('extractor self-test failed: semicolon-only C# record span crossed into the next owner');
  }
  if (memberNameFromUnit('declare const privateBrand: unique symbol;', 'typescript') !== undefined) {
    throw new Error('extractor self-test failed: non-exported TypeScript brand declaration became public');
  }
  if (memberNameFromUnit('readonly [privateBrand]: true;', 'typescript') !== '[privateBrand]') {
    throw new Error('extractor self-test failed: computed TypeScript property lost its symbol name');
  }
  if (memberNameFromUnit('export declare const PUBLIC_TOKEN: unique symbol;', 'typescript') !== 'PUBLIC_TOKEN') {
    throw new Error('extractor self-test failed: exported TypeScript token was omitted');
  }
  const cpp = [
    'class location_store_t {',
    'public:',
    '    virtual result_t compare_exchange_authority(',
    '        key_t key,',
    '        std::stop_token cancellation = {}) = 0;',
    '};',
  ].join('\n');
  const cppMembers = extractMembers({
    block: {
      id: 'cpp:extractor-self-test',
      source: cpp,
      startLine: 1,
      tag: 'cpp',
    },
    categories: [],
    disposition: 'self-test',
    document: 'extractor-self-test',
    language: 'cpp',
    packages: ['extractor-self-test'],
  }).filter(member => member.memberName === 'compare_exchange_authority');
  if (cppMembers.length !== 1
      || !cppMembers[0].normalizedSignature.includes('cancellation ={})')) {
    throw new Error(
      `extractor self-test failed: C++ default initializer became a duplicate method body:`
      + ` ${JSON.stringify(cppMembers.map(member => member.normalizedSignature))}`);
  }
  const exactBaseline = {
    identity: 'cpp|pkg|pkg::publisher_t.publish#valid',
    signatureSha256: 'baseline-valid',
  };
  const exactTarget = {
    identity: 'cpp|pkg|pkg::publisher_t.publish#target-valid',
    signatureSha256: 'target-valid',
  };
  const staleFirst = {
    baselineIdentity: 'cpp|pkg|pkg::publisher_t.publish#stale',
    baselineSignatureSha256: 'baseline-stale',
    targetIdentity: 'cpp|pkg|pkg::publisher_t.publish#target-stale',
    targetSignatureSha256: 'target-stale',
  };
  const validSecond = {
    baselineIdentity: exactBaseline.identity,
    baselineSignatureSha256: exactBaseline.signatureSha256,
    targetIdentity: exactTarget.identity,
    targetSignatureSha256: exactTarget.signatureSha256,
  };
  const ordered = orderOverridesForRefresh(
    [staleFirst, validSecond],
    new Map([[exactBaseline.identity, exactBaseline]]),
    new Map([[exactTarget.identity, exactTarget]]));
  if (ordered[0] !== validSecond) {
    throw new Error('extractor self-test failed: stale override took precedence over an exact overload');
  }
  return 6;
};

const loadConfig = () => JSON.parse(fs.readFileSync(configPath, 'utf8'));
const command = process.argv[2];
if (!['--write', '--check', '--self-test', '--review-candidates', '--refresh-review'].includes(command)) {
  process.stderr.write('usage: generate-v11-public-contract-trace.mjs --write|--check|--self-test|--review-candidates|--refresh-review\n');
  process.exit(2);
}

try {
  const extractorCases = runExtractorSelfTests();
  const config = loadConfig();
  const trace = buildTrace(config, {refreshReview: command === '--refresh-review'});
  if (command === '--refresh-review') {
    const failures = validationFailures(trace);
    if (failures.length > 0) {
      const ownerCounts = (trace.declarationOwners || []).reduce((counts, owner) => {
        counts.set(owner.identity, (counts.get(owner.identity) || 0) + 1);
        return counts;
      }, new Map());
      const duplicateOwners = [...ownerCounts]
        .filter(([, count]) => count > 1)
        .map(([identity, count]) => {
          const records = trace.declarationOwners.filter(owner => owner.identity === identity);
          return `${identity} (${count}) ${records.map(owner =>
            `${owner.exactInterface}:${owner.codeBlockIds.map(id => {
              const block = trace.codeBlocks.find(candidate => candidate.id === id);
              return `${id}@${block?.startLine}:${block?.heading}`;
            }).join(',')}`).join(' | ')}`;
        });
      throw new Error(`${failures.join('\n')}`
        + (duplicateOwners.length ? `\nduplicate owners:\n${duplicateOwners.join('\n')}` : ''));
    }
    config.baseline.memberOverrides.sort((left, right) =>
      left.baselineIdentity.localeCompare(right.baselineIdentity, 'en'));
    fs.writeFileSync(configPath, stableJson(config));
    process.stdout.write(
      `v11 public-contract review refreshed:`
        + ` overrides=${config.baseline.memberOverrides.length}`
        + ` new_members=${config.baseline.reviewedV11FirstMembers.count}`
        + ` new_owners=${config.baseline.reviewedV11FirstOwners.count}`
        + ` removals=${config.baseline.reviewedIntentionalRemovals.count}\n`);
  } else if (command === '--review-candidates') {
    process.stdout.write(`${stableJson({ambiguities: []})}`);
  } else {
    const failures = validationFailures(trace);
    if (failures.length > 0) throw new Error(failures.join('\n'));
    const negativeMutations = runNegativeSelfTests(trace);
    const output = stableJson(trace);
    if (command === '--write') {
      fs.writeFileSync(tracePath, output);
    } else if (command === '--check') {
      if (!fs.existsSync(tracePath)) {
        throw new Error(`missing committed trace: ${path.relative(repositoryRoot, tracePath)}`);
      }
      const committed = fs.readFileSync(tracePath, 'utf8');
      if (committed !== output) {
        throw new Error('public-contract trace is stale; run --write after reviewing the exact-interface change');
      }
      const committedFailures = validationFailures(JSON.parse(committed));
      if (committedFailures.length > 0) throw new Error(committedFailures.join('\n'));
    }
    process.stdout.write(
      `v11 public-contract trace ${command.slice(2)} passed:`
        + ` documents=${trace.summary.documents}`
        + ` code_blocks=${trace.summary.codeBlocks}`
        + ` declaration_owners=${trace.summary.declarationOwners}`
        + ` members=${trace.summary.members}`
        + ` categories=${trace.summary.categories}`
        + ` decisions=${trace.summary.decisions}`
        + ` baseline=${trace.summary.baselineRevision}`
        + ` exact=${trace.summary.exactSignatureMatches}`
        + ` canonical=${trace.summary.canonicalSignatureMatches}`
        + ` overrides=${trace.summary.overrideMatches}`
        + ` unclassified=${trace.summary.unclassifiedMembers}`
        + ` ambiguous=${trace.summary.ambiguousMembers}`
        + ` removals=${trace.summary.intentionalRemovals}`
        + ` unknown_or_unowned=${trace.summary.unknownOrUnowned}`
        + ` negative_mutations=${negativeMutations}`
        + ` extractor_cases=${extractorCases}\n`);
  }
} catch (error) {
  if ((command === '--review-candidates' || command === '--refresh-review')
      && error.code === 'TRACE_MEMBER_AMBIGUITY') {
    process.stdout.write(stableJson({ambiguities: error.candidates}));
  } else {
    process.stderr.write(`${error.stack || error.message}\n`);
    process.exitCode = 1;
  }
}
