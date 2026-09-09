#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const sampleRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repositoryGenerator = path.resolve(sampleRoot, '../../scripts/generate-framework-json-schemas.mjs');

const generator = fs.existsSync(repositoryGenerator)
  ? repositoryGenerator
  : path.join(sampleRoot, 'scripts', 'generate-framework-json-schemas.mjs');

const result = spawnSync(process.execPath, [generator, ...process.argv.slice(2)], {
  cwd: sampleRoot,
  stdio: 'inherit',
  env: process.env
});
if (result.error) throw result.error;
process.exit(result.status ?? 1);
