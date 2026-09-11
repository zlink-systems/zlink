#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { materializeLocalHttpClientArchive } from './local-http-client-package.mjs';

const nodeRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = path.resolve(nodeRoot, '../../..');
const packageManifest = JSON.parse(fs.readFileSync(
  path.join(nodeRoot, 'packages', 'http-client', 'package.json'),
  'utf8'
));
const archive = materializeLocalHttpClientArchive({
  repoRoot,
  version: packageManifest.version,
  configuredRoot: process.env.ZLINK_LOCAL_PACKAGE_ROOT
});

process.stdout.write(`Materialized local HTTP client package ${archive}\n`);
