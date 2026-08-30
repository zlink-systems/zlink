// SPDX-License-Identifier: MPL-2.0

'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const { parseCommonArgs } = require('../perf/common/perf_args');

const packageRoot = path.resolve(__dirname, '../..');
const multiRunner = path.join(packageRoot, 'perf/multi/run_benchmarks.sh');
const multiSource = fs.readFileSync(
    path.join(packageRoot, 'perf/multi/run_benchmarks.ts'),
    'utf8'
);
const multiShell = fs.readFileSync(multiRunner, 'utf8');
const singleSource = fs.readFileSync(
  path.join(packageRoot, 'perf/single/run_benchmarks.ts'),
  'utf8'
);
const singleShell = fs.readFileSync(
  path.join(packageRoot, 'perf/single/run_benchmarks.sh'),
  'utf8'
);
const packageJson = JSON.parse(
  fs.readFileSync(path.join(packageRoot, 'package.json'), 'utf8')
) as { scripts: Record<string, string> };
const defaults = {
  pattern: 'ALL',
  duration: 5,
  msgSizes: [1024],
  resultsDir: 'perf/results',
  transports: ['tcp'],
  clients: 100
};

test('multi runner help documents the supported I/O-thread contract', () => {
  assert.match(multiSource, /--io-threads N\s+Set both server\/client I\/O threads \(default: 4\)\./);
  assert.match(multiSource, /--server-io-threads N\s+Override server I\/O threads \(default: 4\)\./);
  assert.match(multiSource, /--client-io-threads N\s+Override client I\/O threads \(default: 4\)\./);

  const options = parseCommonArgs([
    '--io-threads', '3',
    '--server-io-threads', '5',
    '--client-io-threads', '7'
  ], defaults);
  assert.equal(options.ioThreads, 3);
  assert.equal(options.serverIoThreads, 5);
  assert.equal(options.clientIoThreads, 7);
});

test('Node perf rejects build-dir instead of consuming and ignoring it', () => {
  const expected = /--build-dir is not supported by the Node perf runner/;
  assert.throws(
    () => parseCommonArgs(['--build-dir', 'alternate-build'], defaults),
    expected
  );
  assert.throws(
    () => parseCommonArgs(['--build-dir=alternate-build'], defaults),
    expected
  );

  if (process.platform !== 'win32') {
    const result = spawnSync(multiRunner, ['--build-dir', 'alternate-build'], {
      cwd: packageRoot,
      encoding: 'utf8'
    });
    assert.equal(result.status, 1);
    assert.match(result.stderr, expected);
    assert.doesNotMatch(result.stdout, /> @zlink-systems\/zlink@.* build/);
  }
});

test('Node build modes keep latest-source, reuse, and clean semantics explicit', () => {
  assert.doesNotThrow(() => parseCommonArgs(['--reuse-build'], defaults));
  assert.doesNotThrow(() => parseCommonArgs(['--clean-build'], defaults));
  assert.match(multiSource, /--reuse-build\s+Reuse existing fixed Node outputs; skip rebuild\./);
  assert.match(multiSource, /--clean-build\s+Remove TypeScript and native build outputs, then rebuild\./);
  assert.match(singleSource, /--reuse-build\s+Reuse existing fixed Node outputs; skip rebuild\./);
  assert.match(singleSource, /--clean-build\s+Remove TypeScript and native build outputs, then rebuild\./);
  assert.ok(multiShell.includes('npm run build:incremental'));
  assert.ok(multiShell.includes('npm run rebuild-native'));
  assert.ok(multiShell.includes('rm -rf "$ROOT_DIR/dist" "$ROOT_DIR/dist-tools" "$ROOT_DIR/build"'));
  assert.ok(singleShell.includes('npm run build:incremental'));
  assert.ok(singleShell.includes('npm run rebuild-native'));
  assert.ok(singleShell.includes('rm -rf "$ROOT_DIR/dist" "$ROOT_DIR/dist-tools" "$ROOT_DIR/build"'));
  assert.equal(packageJson.scripts.build, 'npm run clean && npm run build:incremental');
  assert.equal(
    packageJson.scripts['build:incremental'],
    'tsc -p tsconfig.json && node scripts/generate_esm_wrapper.js '
      + '&& tsc -p tsconfig.tools.json && node scripts/link_dist_tools.js'
  );
  assert.ok(multiShell.includes('dist-tools/perf/multi/run_benchmarks.js'));
  assert.ok(multiShell.includes('dist/index.js'));
  assert.ok(multiShell.includes('build/Release/zlink.node'));
  assert.ok(multiShell.includes('prebuilds/$PREBUILD_PLATFORM/zlink.node'));

  if (process.platform !== 'win32') {
    const result = spawnSync(multiRunner, ['--reuse-build', '--clean-build'], {
      cwd: packageRoot,
      encoding: 'utf8'
    });
    assert.equal(result.status, 1);
    assert.match(result.stderr, /--reuse-build and --clean-build are mutually exclusive/);
    assert.doesNotMatch(result.stdout, /> @zlink-systems\/zlink@.* build/);
  }
});
