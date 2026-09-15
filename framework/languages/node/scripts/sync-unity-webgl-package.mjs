#!/usr/bin/env node
// Copies the IIFE browser bundle of @zlink-systems/stream-connector into the
// Unity WebGL UPM package as an emscripten pre-js file.
//
// The bundle is a committed artifact. UPM installs a package by checking out a
// git tag (or by unpacking a tarball); it never runs a Node build, so a file
// that only exists after `npm run build` would be missing for every consumer.
// The cost of committing a generated file is version drift, so `--check` is a
// gate: it regenerates the file and fails when the committed bytes differ from
// the bundle the current source produces.
//
// Unity links `.jspre` files with emscripten's --pre-js and `.jslib` files with
// --js-library. A plain `.js` file under Plugins/WebGL is not linked into the
// build, which is why the bundle is written with the `.jspre` extension.
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const nodeRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = path.resolve(nodeRoot, '../../..');

export const unityPackageRoot = path.join(
  repoRoot,
  'framework/languages/unity/com.zlink.stream-connector.webgl'
);
export const bundleSource = path.join(
  nodeRoot,
  'packages/stream-connector/dist/browser/index.global.js'
);
export const bundleTarget = path.join(
  unityPackageRoot,
  'Plugins/WebGL/zlink-stream-connector.jspre'
);
export const globalName = 'ZlinkStreamConnectorBundle';

export function renderBundle(source) {
  const manifest = JSON.parse(
    fs.readFileSync(path.join(unityPackageRoot, 'package.json'), 'utf8')
  );
  return [
    '// GENERATED FILE - DO NOT EDIT.',
    '// Produced by framework/languages/node/scripts/sync-unity-webgl-package.mjs from',
    '// @zlink-systems/stream-connector (package root, IIFE build).',
    `// Package version: ${manifest.version}`,
    '//',
    '// This is the same TypeScript connector the npm package root ships. The UPM',
    '// adapter adds no wire runtime of its own (stream-connector spec 32 section 11).',
    '',
    source.trimEnd(),
    '',
    '// The emscripten module body and the jslib functions share one closure, so the',
    '// `var` above is already visible to ZlinkStreamConnector.jslib. The globalThis',
    '// assignment is the fallback for emscripten link modes that place pre-js in a',
    '// different scope, and it is what the Node jslib harness reads.',
    `if (typeof globalThis !== 'undefined') { globalThis.${globalName} = ${globalName}; }`,
    ''
  ].join('\n');
}

export function syncUnityWebglPackage({ check = false } = {}) {
  if (!fs.existsSync(bundleSource)) {
    throw new Error(
      `Browser IIFE bundle is missing: ${bundleSource}. Run \`npm run build:browser\` first.`
    );
  }
  const rendered = renderBundle(fs.readFileSync(bundleSource, 'utf8'));
  const current = fs.existsSync(bundleTarget) ? fs.readFileSync(bundleTarget, 'utf8') : undefined;
  if (current === rendered) {
    return { changed: false, target: bundleTarget };
  }
  if (check) {
    throw new Error(
      `${path.relative(repoRoot, bundleTarget)} is stale. Run \`npm run build:browser\` in ` +
      'framework/languages/node and commit the regenerated file.'
    );
  }
  fs.mkdirSync(path.dirname(bundleTarget), { recursive: true });
  fs.writeFileSync(bundleTarget, rendered);
  return { changed: true, target: bundleTarget };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const check = process.argv.includes('--check');
  const result = syncUnityWebglPackage({ check });
  process.stdout.write(
    `${result.changed ? 'Updated' : 'Up to date'}: ${path.relative(repoRoot, result.target)}\n`
  );
}
