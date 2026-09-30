#!/usr/bin/env node
// Perf measures what a user installs: the published @zlink-systems packages, never the repository workspace.
// This is the sample user mode (doc/building/framework-workspace.ko.md section 2), so the sample helper is reused
// with package mode forced; it fails when a package resolved to the workspace or is missing.
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const perfRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const helper = path.resolve(perfRoot, '../samples/scripts/prepare-sample-dependencies.mjs');

process.env.ZLINK_NODE_SAMPLES_PACKAGE_MODE = '1';
const { prepareSampleDependencies } = await import(pathToFileURL(helper).href);
prepareSampleDependencies(perfRoot);
