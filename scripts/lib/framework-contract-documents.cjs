'use strict';

const {createHash} = require('crypto');
const fs = require('fs');
const path = require('path');

const EXACT_ROOT = 'framework/doc/framework/common/spec/server/languages';

function exactDocumentPaths(root, language) {
  const relativeDirectory = path.posix.join(EXACT_ROOT, language, 'interfaces');
  const absoluteDirectory = path.join(root, relativeDirectory);
  if (!fs.existsSync(absoluteDirectory)) return [];
  return fs.readdirSync(absoluteDirectory, { withFileTypes: true })
    .filter(entry => entry.isFile() && entry.name.endsWith('.ko.md'))
    .map(entry => path.posix.join(relativeDirectory, entry.name))
    .sort((left, right) => left.localeCompare(right, 'en'));
}

function headedFencedBlocks(source, acceptedTags = undefined) {
  const accepted = acceptedTags ? new Set(acceptedTags) : undefined;
  const blocks = [];
  let heading = '';
  let ordinal = 0;
  const lines = source.split(/\r?\n/u);
  for (let index = 0; index < lines.length; index += 1) {
    const headingMatch = /^#{1,6}\s+(.+?)\s*#*\s*$/u.exec(lines[index]);
    if (headingMatch) {
      heading = headingMatch[1];
      continue;
    }
    const fenceMatch = /^```([^\s`]*)\s*$/u.exec(lines[index]);
    if (!fenceMatch) continue;
    const tag = fenceMatch[1].toLowerCase();
    const startLine = index + 2;
    const body = [];
    for (index += 1; index < lines.length && !/^```\s*$/u.test(lines[index]); index += 1) {
      body.push(lines[index]);
    }
    if (accepted && !accepted.has(tag)) continue;
    blocks.push({
      heading,
      ordinal: ordinal++,
      source: body.join('\n').replace(/[ \t]+$/gmu, '').trim(),
      startLine,
      tag,
    });
  }
  return blocks;
}

function fencedBlocks(source, acceptedTags = undefined) {
  return headedFencedBlocks(source, acceptedTags);
}

function exactInterfaceBlockRole(config, languageConfig, document, block) {
  const digest = createHash('sha256').update(block.source).digest('hex');
  const overrides = (config.blockRoleOverrides || [])
    .filter(rule => rule.document === document && rule.sha256 === digest);
  if (overrides.length > 1) {
    throw new Error(`code block has multiple role overrides: ${document}:${block.startLine}`);
  }
  if (overrides.length === 1) return overrides[0];
  if (!languageConfig.acceptedCodeTags.includes(block.tag)) {
    return {
      role: 'documentation-support',
      reason: `The ${block.tag || 'untagged'} fence is documentation support, not ${languageConfig.id} package source.`,
    };
  }
  if (/(?:^|\s)\uc608\uc81c(?:\s|$)/u.test(block.heading)
      || /\/interfaces\/(?:[0-9]+-)?examples\.ko\.md$/u.test(document)) {
    return {
      role: 'application-example',
      reason: 'The fenced block demonstrates application use and is not a package-owned declaration.',
    };
  }
  return {role: 'package-contract'};
}

function readExactContract(root, language, acceptedTags = undefined) {
  const documents = exactDocumentPaths(root, language);
  const entries = documents.map(relative => {
    const source = fs.readFileSync(path.join(root, relative), 'utf8');
    return { relative, source, blocks: fencedBlocks(source, acceptedTags) };
  });
  return {
    documents,
    entries,
    source: entries.map(entry => entry.source).join('\n'),
    code: entries.flatMap(entry => entry.blocks.map(block => block.source)).join('\n'),
  };
}

function codeFenceFailures(root, documents) {
  const failures = [];
  for (const relative of documents) {
    const lines = fs.readFileSync(path.join(root, relative), 'utf8').split(/\r?\n/);
    const fenceCount = lines.filter(line => /^```/.test(line)).length;
    if (fenceCount % 2 !== 0) failures.push(`${relative}: unbalanced code fence`);
  }
  return failures;
}

function markdownAnchors(absolute, cache) {
  if (cache.has(absolute)) return cache.get(absolute);
  const anchors = new Set();
  const occurrences = new Map();
  let inFence = false;
  for (const line of fs.readFileSync(absolute, 'utf8').split(/\r?\n/)) {
    if (/^```/.test(line)) {
      inFence = !inFence;
      continue;
    }
    if (inFence) continue;
    const heading = /^#{1,6}\s+(.+?)\s*#*\s*$/.exec(line)?.[1];
    if (!heading) continue;
    const explicit = /\{#([^}\s]+)[^}]*\}\s*$/u.exec(heading);
    if (explicit) {
      anchors.add(explicit[1].toLocaleLowerCase('en-US'));
      continue;
    }
    const base = heading
      .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
      .replace(/<[^>]+>/g, '')
      .replace(/[`*_~]/g, '')
      .toLocaleLowerCase('en-US')
      .replace(/[^\p{L}\p{N}\s_-]/gu, '')
      .trim()
      .replace(/\s+/g, '-');
    const occurrence = occurrences.get(base) || 0;
    occurrences.set(base, occurrence + 1);
    anchors.add(occurrence === 0 ? base : `${base}-${occurrence}`);
  }
  cache.set(absolute, anchors);
  return anchors;
}

function relativeMarkdownLinkFailures(root, documents) {
  const failures = [];
  const anchorCache = new Map();
  for (const relative of documents) {
    const absolute = path.join(root, relative);
    const lines = fs.readFileSync(absolute, 'utf8').split(/\r?\n/);
    let inFence = false;
    for (let index = 0; index < lines.length; index += 1) {
      const line = lines[index];
      if (/^```/.test(line)) {
        inFence = !inFence;
        continue;
      }
      if (inFence) continue;
      for (const match of line.matchAll(/\[[^\]]*\]\(([^)]+)\)/g)) {
        let destination = match[1].trim();
        if (destination.startsWith('<') && destination.endsWith('>')) {
          destination = destination.slice(1, -1);
        }
        if (/^(?:https?:|mailto:)/.test(destination)) continue;
        const hashIndex = destination.indexOf('#');
        const relativeTarget = hashIndex < 0 ? destination : destination.slice(0, hashIndex);
        const fragment = hashIndex < 0 ? '' : destination.slice(hashIndex + 1);
        let decodedTarget;
        try {
          decodedTarget = decodeURIComponent(relativeTarget);
        } catch {
          failures.push(`${relative}:${index + 1}: invalid link encoding: ${destination}`);
          continue;
        }
        const target = decodedTarget.length === 0
          ? absolute
          : path.resolve(path.dirname(absolute), decodedTarget);
        if (!fs.existsSync(target)) {
          failures.push(`${relative}:${index + 1}: missing link target: ${destination}`);
          continue;
        }
        if (fragment && target.endsWith('.md')) {
          let decodedFragment;
          try {
            decodedFragment = decodeURIComponent(fragment).toLocaleLowerCase('en-US');
          } catch {
            failures.push(`${relative}:${index + 1}: invalid anchor encoding: ${destination}`);
            continue;
          }
          if (!markdownAnchors(target, anchorCache).has(decodedFragment)) {
            failures.push(`${relative}:${index + 1}: missing link anchor: ${destination}`);
          }
        }
      }
    }
  }
  return failures;
}

function publicDeclarationNames(language, source) {
  const patterns = [];
  if (language === 'dotnet') {
    patterns.push(
      /^public\s+(?:(?:sealed|abstract|readonly|partial|static)\s+)*(?:class|struct|interface|enum|record(?:\s+struct|\s+class)?)\s+([A-Za-z_]\w*)/gm,
      /^public\s+delegate\s+[^;(]*?\s+([A-Za-z_]\w*)\s*\(/gm);
  } else if (language === 'cpp') {
    patterns.push(
      /^(?:template\s*<[^\n>]+>\s*)?(?:class|struct|enum\s+class|enum)\s+([A-Za-z_]\w*)\b/gm,
      /^using\s+([A-Za-z_]\w*)\s*=/gm);
  } else if (language === 'java') {
    patterns.push(/^public\s+(?:(?:sealed|non-sealed|final|abstract|static)\s+)*(?:class|interface|record|enum|@interface)\s+([A-Za-z_][\w.$]*)/gm);
  } else if (language === 'kotlin') {
    patterns.push(
      /^public\s+(?:(?:sealed|non-sealed|final|abstract|static)\s+)*(?:class|interface|record|enum|@interface)\s+([A-Za-z_][\w.$]*)/gm,
      /^(?:public\s+)?(?:(?:data|sealed|enum|value|fun)\s+)*(?:class|interface|object|typealias)\s+([A-Za-z_]\w*)(?=\s|<|:|\{|=|\()/gm);
  } else if (language === 'node') {
    patterns.push(/^export\s+(?:declare\s+)?(?:interface|class|enum|type|const|function)\s+([A-Za-z_]\w*)/gm);
  }
  const names = new Set();
  for (const pattern of patterns) {
    for (const match of source.matchAll(pattern)) names.add(match[1]);
  }
  return names;
}

function duplicateDeclarationOwnerFailures(language, contract) {
  const owners = new Map();
  for (const entry of contract.entries) {
    const code = entry.blocks
      .filter(block => block.role === 'package-contract')
      .map(block => block.source)
      .join('\n');
    for (const name of publicDeclarationNames(language, code)) {
      if (!owners.has(name)) owners.set(name, new Set());
      owners.get(name).add(entry.relative);
    }
  }
  return [...owners]
    .filter(([, documents]) => documents.size > 1)
    .map(([name, documents]) => `${language}: ${name}: ${[...documents].sort().join(', ')}`);
}

function duplicateCodeBlockFailures(language, contract) {
  const owners = new Map();
  for (const entry of contract.entries) {
    for (const block of entry.blocks) {
      if (!block.source) continue;
      if (!owners.has(block.source)) owners.set(block.source, new Set());
      owners.get(block.source).add(entry.relative);
    }
  }
  return [...owners]
    .filter(([, documents]) => documents.size > 1)
    .map(([, documents]) => `${language}: ${[...documents].sort().join(', ')}`);
}

module.exports = {
  codeFenceFailures,
  duplicateCodeBlockFailures,
  duplicateDeclarationOwnerFailures,
  exactInterfaceBlockRole,
  exactDocumentPaths,
  fencedBlocks,
  headedFencedBlocks,
  publicDeclarationNames,
  readExactContract,
  relativeMarkdownLinkFailures,
};

if (require.main === module) {
  const [command, root, language, destination] = process.argv.slice(2);
  if (command === 'list' && root && language) {
    process.stdout.write(`${exactDocumentPaths(root, language).join('\n')}\n`);
  } else if (command === 'aggregate' && root && language && destination) {
    const contract = readExactContract(root, language);
    if (contract.documents.length === 0) process.exitCode = 1;
    else fs.writeFileSync(destination, contract.source);
  } else if (command === 'check-links' && root && language) {
    const failures = relativeMarkdownLinkFailures(root, exactDocumentPaths(root, language));
    if (failures.length) {
      process.stderr.write(`${failures.join('\n')}\n`);
      process.exitCode = 1;
    }
  } else {
    process.stderr.write('usage: framework-contract-documents.cjs list|aggregate|check-links ROOT LANGUAGE [DESTINATION]\n');
    process.exitCode = 2;
  }
}
