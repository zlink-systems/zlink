#!/usr/bin/env node

// SPDX-License-Identifier: MPL-2.0

import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, '..');
const configPath = path.join(repositoryRoot,
  'framework/doc/contract-inventory/route-mesh-v11-spec-review-candidate.config.json');
const candidatePath = process.env.ZLINK_V11_SPEC_CANDIDATE_PATH
  ? path.resolve(process.env.ZLINK_V11_SPEC_CANDIDATE_PATH)
  : path.join(repositoryRoot, '.artifacts/v11/spec-review-candidate.json');

const sha256 = value => createHash('sha256').update(value).digest('hex');
const stableJson = value => `${JSON.stringify(value, null, 2)}\n`;
const uniqueSorted = values => [...new Set(values)]
  .sort((left, right) => left.localeCompare(right, 'en'));

function runGit(arguments_, options = {}) {
  const result = spawnSync('git', ['-C', repositoryRoot, ...arguments_], {
    encoding: options.encoding ?? 'utf8',
    maxBuffer: 128 * 1024 * 1024,
  });
  if (result.error) throw new Error(`git ${arguments_[0]} could not start: ${result.error.message}`);
  if (result.status !== 0 && !options.allowFailure) {
    throw new Error(`git ${arguments_.join(' ')} failed: ${String(result.stderr).trim()}`);
  }
  return result;
}

function validatePath(value, label) {
  if (typeof value !== 'string' || value.length === 0 || path.isAbsolute(value)
      || value.split('/').includes('..') || value.includes('\\')) {
    throw new Error(`${label} must be a normalized repository-relative path: ${value}`);
  }
}

function readConfig() {
  const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  if (config.schema !== 'zlink-v11-spec-review-candidate-config-v1'
      || config.candidateSchema !== 'zlink-v11-spec-review-candidate-v2') {
    throw new Error('SPEC candidate config schema is unsupported');
  }
  if (!/^[0-9a-f]{40}$/u.test(config.baseRevision)) {
    throw new Error('SPEC candidate baseRevision must be a full Git object ID');
  }
  for (const key of ['scopeIds', 'ownedPaths', 'directInputPaths', 'requiredChecks']) {
    if (!Array.isArray(config[key]) || config[key].length === 0
        || uniqueSorted(config[key]).length !== config[key].length) {
      throw new Error(`SPEC candidate config ${key} must be a non-empty unique array`);
    }
  }
  for (const [index, value] of [...config.ownedPaths, ...config.directInputPaths].entries()) {
    validatePath(value, `SPEC candidate path ${index}`);
  }
  runGit(['cat-file', '-e', `${config.baseRevision}^{commit}`]);
  return config;
}

const under = (file, owner) => file === owner || file.startsWith(`${owner}/`);

function currentFiles() {
  const result = runGit(['ls-files', '--cached', '--others', '--exclude-standard', '-z'], {
    encoding: 'buffer',
  });
  return result.stdout.toString('utf8').split('\0').filter(Boolean)
    .filter(file => fs.existsSync(path.join(repositoryRoot, file)));
}

function baseFiles(baseRevision) {
  const result = runGit(['ls-tree', '-r', '--name-only', '-z', baseRevision], {
    encoding: 'buffer',
  });
  return result.stdout.toString('utf8').split('\0').filter(Boolean);
}

function currentContent(file) {
  const absolute = path.join(repositoryRoot, file);
  const stat = fs.lstatSync(absolute);
  if (stat.isSymbolicLink()) {
    return {mode: '120000', content: Buffer.from(fs.readlinkSync(absolute), 'utf8')};
  }
  if (!stat.isFile()) throw new Error(`candidate path is not a file or symlink: ${file}`);
  return {
    mode: (stat.mode & 0o111) === 0 ? '100644' : '100755',
    content: fs.readFileSync(absolute),
  };
}

function baseContent(baseRevision, file) {
  const tree = runGit(['ls-tree', baseRevision, '--', file]);
  const match = /^(\d{6})\s+\w+\s+[0-9a-f]+\t/u.exec(tree.stdout);
  if (!match) return undefined;
  const blob = runGit(['show', `${baseRevision}:${file}`], {encoding: 'buffer'});
  return {mode: match[1], content: Buffer.from(blob.stdout)};
}

function fileRecord(baseRevision, file, inclusions) {
  const current = fs.existsSync(path.join(repositoryRoot, file)) ? currentContent(file) : undefined;
  const base = baseContent(baseRevision, file);
  const currentSha = current ? sha256(current.content) : null;
  const baseSha = base ? sha256(base.content) : null;
  let status;
  if (!base) status = 'added';
  else if (!current) status = 'deleted';
  else if (baseSha === currentSha && base.mode === current.mode) status = 'unchanged';
  else status = 'modified';
  return {
    path: file,
    status,
    mode: current?.mode ?? null,
    contentSha256: currentSha,
    baseMode: base?.mode ?? null,
    baseContentSha256: baseSha,
    inclusions,
  };
}

function generateCandidate(config) {
  const current = currentFiles();
  const base = baseFiles(config.baseRevision);
  const all = uniqueSorted([...current, ...base]);
  const records = [];
  for (const file of all) {
    const owned = config.ownedPaths.some(owner => under(file, owner));
    const direct = config.directInputPaths.some(owner => under(file, owner));
    if (!owned && !direct) continue;
    const record = fileRecord(config.baseRevision, file, [
      ...(owned ? ['owned-change'] : []),
      ...(direct ? ['direct-check-input'] : []),
    ]);
    if (record.status !== 'unchanged' || direct) records.push(record);
  }
  records.sort((left, right) => left.path.localeCompare(right.path, 'en'));
  const changedPathCount = records.filter(record => record.status !== 'unchanged').length;
  const directInputPathCount = records.filter(record =>
    record.inclusions.includes('direct-check-input')).length;
  const aggregateInput = JSON.stringify(records);
  return {
    schema: config.candidateSchema,
    baseRevision: config.baseRevision,
    scopeIds: config.scopeIds,
    ownedPaths: config.ownedPaths,
    directInputPaths: config.directInputPaths,
    requiredChecks: config.requiredChecks,
    digestContract: {
      content: 'SHA-256 of the exact file bytes; symlinks hash their link text',
      aggregate: 'SHA-256 of UTF-8 JSON.stringify(files), with files sorted by repository path',
      deletedFile: 'contentSha256 and mode are null; baseContentSha256 and baseMode identify the removed input',
    },
    pathCount: records.length,
    changedPathCount,
    directInputPathCount,
    aggregateSha256: sha256(Buffer.from(aggregateInput, 'utf8')),
    files: records,
  };
}

function selfTest(config) {
  const first = generateCandidate(config);
  const second = generateCandidate(config);
  if (stableJson(first) !== stableJson(second)) {
    throw new Error('SPEC candidate generation is not deterministic');
  }
  if (first.pathCount !== first.files.length
      || first.changedPathCount !== first.files.filter(file => file.status !== 'unchanged').length
      || first.directInputPathCount !== first.files.filter(file =>
        file.inclusions.includes('direct-check-input')).length) {
    throw new Error('SPEC candidate derived counts differ from the exact file list');
  }
  const expectedAggregate = sha256(Buffer.from(JSON.stringify(first.files), 'utf8'));
  if (first.aggregateSha256 !== expectedAggregate) {
    throw new Error('SPEC candidate aggregate does not match the exact file list');
  }
  for (const direct of config.directInputPaths) {
    if (!first.files.some(file => under(file.path, direct))) {
      throw new Error(`SPEC candidate omits direct check input: ${direct}`);
    }
  }
  const mutated = structuredClone(first.files);
  if (mutated.length === 0) throw new Error('SPEC candidate self-test has no scoped paths');
  mutated[0].contentSha256 = mutated[0].contentSha256 === null
    ? '0'.repeat(64)
    : `${mutated[0].contentSha256.slice(0, 63)}${mutated[0].contentSha256.endsWith('0') ? '1' : '0'}`;
  if (sha256(Buffer.from(JSON.stringify(mutated), 'utf8')) === first.aggregateSha256) {
    throw new Error('SPEC candidate digest mutation was not detected');
  }
  console.log(`SPEC candidate self-test passed: paths=${first.pathCount}, changed=${first.changedPathCount}, direct-inputs=${first.directInputPathCount}`);
}

function usage() {
  console.error('usage: generate-v11-spec-review-candidate.mjs --write|--check|--self-test');
}

const mode = process.argv[2];
if (!['--write', '--check', '--self-test'].includes(mode)) {
  usage();
  process.exitCode = 2;
} else {
  try {
    const config = readConfig();
    if (mode === '--self-test') {
      selfTest(config);
    } else {
      const candidate = generateCandidate(config);
      if (mode === '--write') {
        fs.mkdirSync(path.dirname(candidatePath), {recursive: true});
        fs.writeFileSync(candidatePath, stableJson(candidate));
        console.log(`wrote ${path.relative(repositoryRoot, candidatePath)}: paths=${candidate.pathCount}, changed=${candidate.changedPathCount}, aggregate=${candidate.aggregateSha256}`);
      } else {
        if (!fs.existsSync(candidatePath)) throw new Error(`SPEC candidate is missing: ${candidatePath}`);
        const actual = fs.readFileSync(candidatePath, 'utf8');
        const expected = stableJson(candidate);
        if (actual !== expected) {
          throw new Error('SPEC candidate differs from the current snapshot; run with --write');
        }
        console.log(`verified ${path.relative(repositoryRoot, candidatePath)}: paths=${candidate.pathCount}, changed=${candidate.changedPathCount}, aggregate=${candidate.aggregateSha256}`);
      }
    }
  } catch (error) {
    console.error(error.stack ?? error.message);
    process.exitCode = 1;
  }
}
