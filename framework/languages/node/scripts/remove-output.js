#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const nodeRoot = path.resolve(__dirname, '..');
const requested = process.argv.slice(2);
const dryRun = requested.includes('--dry-run');
const patterns = requested.filter((value) => value !== '--dry-run');

if (patterns.length === 0) {
  throw new Error('Usage: node scripts/remove-output.js [--dry-run] <relative-path> [...]');
}

for (const target of expand(patterns)) {
  const resolved = path.resolve(nodeRoot, target);
  const relative = path.relative(nodeRoot, resolved);
  if (relative === '' || relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
    throw new Error(`Refusing to remove path outside the Node workspace: ${target}`);
  }
  if (dryRun) {
    process.stdout.write(`${relative.split(path.sep).join('/')}\n`);
  } else {
    fs.rmSync(resolved, { recursive: true, force: true });
  }
}

function expand(values) {
  const expanded = [];
  for (const value of values) {
    if (value === 'packages/*/dist') {
      const packagesRoot = path.join(nodeRoot, 'packages');
      for (const entry of fs.readdirSync(packagesRoot, { withFileTypes: true })) {
        if (entry.isDirectory()) expanded.push(path.join('packages', entry.name, 'dist'));
      }
    } else {
      expanded.push(value);
    }
  }
  return expanded;
}
