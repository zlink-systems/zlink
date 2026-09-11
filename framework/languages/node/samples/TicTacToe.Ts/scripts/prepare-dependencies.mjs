#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { pathToFileURL } from 'node:url';

const sampleRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repositoryHelper = path.resolve(sampleRoot, '../scripts/prepare-sample-dependencies.mjs');

if (fs.existsSync(repositoryHelper)) {
  const { prepareSampleDependencies } = await import(pathToFileURL(repositoryHelper).href);
  prepareSampleDependencies(sampleRoot);
} else {
  const manifest = JSON.parse(fs.readFileSync(path.join(sampleRoot, 'package.json'), 'utf8'));
  for (const name of Object.keys(manifest.dependencies ?? {}).filter((value) => value.startsWith('@zlink-systems/'))) {
    const installed = path.join(sampleRoot, 'node_modules', ...name.split('/'));
    if (!fs.existsSync(installed)) throw new Error(`Package mode requires ${name}. Run npm install first.`);
  }
  process.stdout.write(`sample_dependency_mode=package sample=${manifest.name}\n`);
}
