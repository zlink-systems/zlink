#!/usr/bin/env node

import { createHash } from 'node:crypto';
import {
  existsSync,
  lstatSync,
  readFileSync,
  readlinkSync,
  writeFileSync
} from 'node:fs';
import { dirname, isAbsolute, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');

function fail(message) {
  throw new Error(message);
}

function sha256(value) {
  return createHash('sha256').update(value).digest('hex');
}

function bytes(path) {
  return lstatSync(path).isSymbolicLink()
    ? Buffer.from(readlinkSync(path), 'utf8')
    : readFileSync(path);
}

function git(args, binary = false) {
  const result = spawnSync('git', args, {
    cwd: repositoryRoot,
    encoding: binary ? null : 'utf8',
    maxBuffer: 128 * 1024 * 1024
  });
  if (result.status !== 0) {
    fail(`git ${args.join(' ')} failed: ${binary ? result.stderr.toString('utf8') : result.stderr}`);
  }
  return result.stdout;
}

function lines(value) {
  return value.trim().split('\n').filter(Boolean);
}

function parse(argv) {
  const options = { ownedPaths: [], directInputs: [] };
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (value === undefined) fail(`missing value for ${key}`);
    switch (key) {
      case '--id':
        options.ledgerId = value;
        break;
      case '--base':
        options.baseRevision = value;
        break;
      case '--owned-path':
        options.ownedPaths.push(value);
        break;
      case '--input':
        options.directInputs.push(value);
        break;
      case '--output':
        options.output = value;
        break;
      default:
        fail(`unknown argument: ${key}`);
    }
  }
  if (!options.ledgerId || !options.baseRevision || !options.output || options.ownedPaths.length === 0) {
    fail('usage: create-ledger-candidate.mjs --id <ledger-id> --base <revision> --owned-path <path> [--owned-path <path>] [--input <path>] --output <absolute.json>');
  }
  if (!isAbsolute(options.output) || !options.output.endsWith('.json')) {
    fail('--output must be an absolute .json path');
  }
  options.ownedPaths = [...new Set(options.ownedPaths)].sort();
  options.directInputs = [...new Set(options.directInputs)].sort();
  return options;
}

function safePath(path) {
  return path.length > 0
    && !isAbsolute(path)
    && !path.includes('\\')
    && !path.split('/').includes('..');
}

function basePaths(baseRevision, ownedPaths) {
  return new Set(lines(git([
    'ls-tree', '-r', '--name-only', baseRevision, '--', ...ownedPaths
  ])));
}

function currentPaths(ownedPaths) {
  return new Set([
    ...lines(git(['ls-files', '--', ...ownedPaths])),
    ...lines(git(['ls-files', '--others', '--exclude-standard', '--', ...ownedPaths]))
  ].filter(path => existsSync(resolve(repositoryRoot, path))));
}

function currentMode(path) {
  const absolute = resolve(repositoryRoot, path);
  const executable = (lstatSync(absolute).mode & 0o111) !== 0;
  return executable ? '100755' : '100644';
}

function baseMode(baseRevision, path) {
  const entry = lines(git(['ls-tree', baseRevision, '--', path]))[0];
  if (entry === undefined) fail(`base mode is missing: ${path}`);
  return entry.split(/\s+/u)[0];
}

function createFileEntries(baseRevision, ownedPaths) {
  const base = basePaths(baseRevision, ownedPaths);
  const current = currentPaths(ownedPaths);
  const paths = [...new Set([...base, ...current])].sort();
  const files = [];
  for (const path of paths) {
    const inBase = base.has(path);
    const inCurrent = current.has(path);
    if (!inCurrent) {
      files.push({
        path,
        status: 'deleted',
        mode: null,
        contentSha256: null,
        baseContentSha256: sha256(git(['show', `${baseRevision}:${path}`], true))
      });
      continue;
    }
    const absolute = resolve(repositoryRoot, path);
    const contentSha256 = sha256(bytes(absolute));
    if (!inBase) {
      files.push({
        path,
        status: 'added',
        mode: currentMode(path),
        contentSha256,
        baseContentSha256: null
      });
      continue;
    }
    const baseBytes = git(['show', `${baseRevision}:${path}`], true);
    const baseContentSha256 = sha256(baseBytes);
    const mode = currentMode(path);
    if (contentSha256 === baseContentSha256 && mode === baseMode(baseRevision, path)) continue;
    files.push({
      path,
      status: 'modified',
      mode,
      contentSha256,
      baseContentSha256
    });
  }
  if (files.length === 0) fail('candidate has no changed files');
  return files;
}

function main() {
  const options = parse(process.argv.slice(2));
  if (!options.ownedPaths.every(safePath) || !options.directInputs.every(safePath)) {
    fail('owned paths and direct inputs must be safe repository-relative paths');
  }
  const baseRevision = git(['rev-parse', `${options.baseRevision}^{commit}`]).trim();
  const files = createFileEntries(baseRevision, options.ownedPaths);
  const directInputs = options.directInputs.map(path => {
    const absolute = resolve(repositoryRoot, path);
    if (!existsSync(absolute)) fail(`direct input is missing: ${path}`);
    return { path, contentSha256: sha256(bytes(absolute)) };
  });
  const candidate = {
    schema: 'zlink-v11-ledger-candidate-v1',
    ledgerId: options.ledgerId,
    baseRevision,
    ownedPaths: options.ownedPaths,
    directInputs,
    pathCount: files.length,
    aggregateSha256: sha256(JSON.stringify(files)),
    files
  };
  const owned = {
    schema: 'zlink-v11-owned-paths-v1',
    ledgerId: options.ledgerId,
    ownedPaths: options.ownedPaths,
    contentSha256: sha256(JSON.stringify(options.ownedPaths))
  };
  writeFileSync(options.output, `${JSON.stringify(candidate, null, 2)}\n`);
  const ownedPath = `${options.output.slice(0, -'.json'.length)}-owned-paths.json`;
  writeFileSync(ownedPath, `${JSON.stringify(owned, null, 2)}\n`);
  process.stdout.write(
    `candidate created: ${options.ledgerId} files=${files.length} output=${options.output}\n`
  );
}

try {
  main();
} catch (error) {
  process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
  process.exitCode = 2;
}
