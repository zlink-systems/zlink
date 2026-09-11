'use strict';

const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { pathToFileURL } = require('node:url');
const zlib = require('node:zlib');

const nodeRoot = path.resolve(__dirname, '../..');
const scriptsRoot = path.join(nodeRoot, 'scripts');
const repositoryRoot = path.resolve(nodeRoot, '../../..');
const frameworkVersion = readVersion(
  path.join(nodeRoot, 'VERSION'),
  'ZLINK_FRAMEWORK_VERSION'
);

test('npm test and release scripts use platform-neutral Node entry points', () => {
  const packageJson = JSON.parse(fs.readFileSync(path.join(nodeRoot, 'package.json'), 'utf8'));
  const gatedScripts = [
    'clean',
    'test',
    'verify:m5-foundation',
    'verify:m6a-runtime',
    'verify:m6b-runtime',
    'verify:m6c-runtime',
    'verify:samples',
    'verify:cross-language',
    'verify:release'
  ];

  for (const name of gatedScripts) {
    assert.doesNotMatch(packageJson.scripts[name], /(^|\s)rm\s+-rf(?:\s|$)/, name);
    assert.doesNotMatch(packageJson.scripts[name], /\.sh(?:\s|$)/, name);
  }
  assert.equal(packageJson.scripts['verify:samples'], 'node scripts/run-samples-gate.js');
  assert.equal(packageJson.scripts['verify:cross-language'], 'node scripts/run-cross-language-smoke.js');
  assert.equal(
    packageJson.scripts['local-package:http-client'],
    'node scripts/materialize-local-http-client-package.mjs'
  );
});

test('Windows framework build materializes and consumes the stable HTTP client archive', () => {
  const script = fs.readFileSync(path.join(nodeRoot, 'build-windows.ps1'), 'utf8');
  assert.match(script, /scripts\/materialize-local-http-client-package\.mjs/);
  assert.match(script, /\$bindingPackage, \$materializedHttpPackage/);
  assert.doesNotMatch(script, /\$bindingPackage, \$httpPackage\s*\)/);
});

test('Windows local binding candidates prefer the canonical Windows npm publisher', async () => {
  const moduleUrl = pathToFileURL(path.join(scriptsRoot, 'local-binding-package.mjs'));
  const { localBindingArchiveCandidates } = await import(moduleUrl.href);
  const versionSource = fs.readFileSync(path.join(repositoryRoot, 'bindings', 'node', 'VERSION'), 'utf8');
  const versionMatch = /^ZLINK_BINDING_VERSION=(.+)$/m.exec(versionSource);
  assert.notEqual(versionMatch, null, 'bindings/node/VERSION must define ZLINK_BINDING_VERSION');
  const version = versionMatch[1].trim();
  const filename = `zlink-systems-zlink-${version}.tgz`;

  assert.deepEqual(
    localBindingArchiveCandidates({ repoRoot: repositoryRoot, version, platform: 'win32' }),
    [
      path.join(repositoryRoot, '.artifacts', 'windows', 'npm', filename)
    ]
  );
  assert.deepEqual(
    localBindingArchiveCandidates({
      repoRoot: repositoryRoot,
      version,
      configuredRoot: path.join(repositoryRoot, 'custom-packages'),
      platform: 'win32'
    }),
    [
      path.join(repositoryRoot, 'custom-packages', 'npm', filename),
      path.join(repositoryRoot, 'custom-packages', filename)
    ]
  );
  assert.deepEqual(
    localBindingArchiveCandidates({ repoRoot: repositoryRoot, version, platform: 'linux' }),
    [
      path.join(repositoryRoot, '.artifacts', 'wsl', 'npm', filename)
    ]
  );
});

test('HTTP client manifest and lock use one platform-neutral materialized archive', () => {
  const packageJson = JSON.parse(fs.readFileSync(path.join(nodeRoot, 'package.json'), 'utf8'));
  const packageLock = JSON.parse(fs.readFileSync(path.join(nodeRoot, 'package-lock.json'), 'utf8'));
  const expected = `file:../../../.artifacts/node-install/npm/zlink-systems-http-client-${frameworkVersion}.tgz`;

  assert.equal(packageJson.dependencies['@zlink-systems/http-client'], expected);
  assert.equal(packageLock.packages[''].dependencies['@zlink-systems/http-client'], expected);
  assert.equal(packageLock.packages['node_modules/@zlink-systems/http-client'].resolved, expected);
});

test('HTTP client materializer selects the host publisher and replaces stale staging atomically', async (t) => {
  const { materializeLocalHttpClientArchive, stagedHttpClientArchive } = await importHttpPackageModule();
  const repoRoot = temporaryRepo(t);
  const version = '1.2.3';
  const source = path.join(
    repoRoot,
    '.artifacts',
    'windows',
    'npm',
    `zlink-systems-http-client-${version}.tgz`
  );
  const destination = stagedHttpClientArchive({ repoRoot, version });
  writeNpmArchive(source, { name: '@zlink-systems/http-client', version });
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.writeFileSync(destination, 'stale package');

  const actual = materializeLocalHttpClientArchive({ repoRoot, version, platform: 'win32' });

  assert.equal(actual, destination);
  assert.equal(sha256(destination), sha256(source));
  assert.deepEqual(
    fs.readdirSync(path.dirname(destination)),
    [path.basename(destination)]
  );
});

test('HTTP client materializer never falls back to the opposite platform and removes stale staging', async (t) => {
  const { materializeLocalHttpClientArchive, stagedHttpClientArchive } = await importHttpPackageModule();
  const repoRoot = temporaryRepo(t);
  const version = '1.2.3';
  const wslArchive = path.join(
    repoRoot,
    '.artifacts',
    'wsl',
    'npm',
    `zlink-systems-http-client-${version}.tgz`
  );
  const destination = stagedHttpClientArchive({ repoRoot, version });
  writeNpmArchive(wslArchive, { name: '@zlink-systems/http-client', version });
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.writeFileSync(destination, 'stale package');

  assert.throws(
    () => materializeLocalHttpClientArchive({ repoRoot, version, platform: 'win32' }),
    /Local HTTP client package was not found/
  );
  assert.equal(fs.existsSync(destination), false);
});

test('HTTP client materializer accepts explicit root/npm and root candidates', async (t) => {
  const { localHttpClientArchiveCandidates, materializeLocalHttpClientArchive } = await importHttpPackageModule();
  const repoRoot = temporaryRepo(t);
  const configuredRoot = path.join(repoRoot, 'custom-packages');
  const version = '1.2.3';
  const filename = `zlink-systems-http-client-${version}.tgz`;

  assert.deepEqual(
    localHttpClientArchiveCandidates({ repoRoot, version, configuredRoot, platform: 'win32' }),
    [path.join(configuredRoot, 'npm', filename), path.join(configuredRoot, filename)]
  );
  assert.deepEqual(
    localHttpClientArchiveCandidates({ repoRoot, version, platform: 'linux' }),
    [path.join(repoRoot, '.artifacts', 'wsl', 'npm', filename)]
  );

  const npmArchive = path.join(configuredRoot, 'npm', filename);
  const rootArchive = path.join(configuredRoot, filename);
  writeNpmArchive(npmArchive, { name: '@zlink-systems/http-client', version, source: 'npm' });
  writeNpmArchive(rootArchive, { name: '@zlink-systems/http-client', version, source: 'root' });
  let destination = materializeLocalHttpClientArchive({
    repoRoot,
    version,
    configuredRoot,
    platform: 'win32'
  });
  assert.equal(sha256(destination), sha256(npmArchive));

  fs.rmSync(npmArchive);
  destination = materializeLocalHttpClientArchive({
    repoRoot,
    version,
    configuredRoot,
    platform: 'win32'
  });
  assert.equal(sha256(destination), sha256(rootArchive));
});

test('HTTP client materializer rejects empty, wrong-name, and wrong-version packages without leaving stale staging', async (t) => {
  const { materializeLocalHttpClientArchive, stagedHttpClientArchive } = await importHttpPackageModule();
  const cases = [
    { label: 'empty', manifest: null, expected: /non-empty file/ },
    { label: 'wrong name', manifest: { name: '@example/wrong', version: '1.2.3' }, expected: /package name/ },
    { label: 'wrong version', manifest: { name: '@zlink-systems/http-client', version: '9.9.9' }, expected: /package version/ }
  ];

  for (const entry of cases) {
    const repoRoot = path.join(temporaryRepo(t), entry.label.replace(' ', '-'));
    const version = '1.2.3';
    const source = path.join(
      repoRoot,
      '.artifacts',
      'windows',
      'npm',
      `zlink-systems-http-client-${version}.tgz`
    );
    const destination = stagedHttpClientArchive({ repoRoot, version });
    fs.mkdirSync(path.dirname(source), { recursive: true });
    if (entry.manifest === null) fs.writeFileSync(source, '');
    else writeNpmArchive(source, entry.manifest);
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    fs.writeFileSync(destination, 'stale package');

    assert.throws(
      () => materializeLocalHttpClientArchive({ repoRoot, version, platform: 'win32' }),
      entry.expected,
      entry.label
    );
    assert.equal(fs.existsSync(destination), false, entry.label);
  }
});

test('output cleanup dry-run expands workspace paths and rejects parent paths', () => {
  const dryRun = runNode('remove-output.js', ['dist', 'packages/*/dist', '--dry-run']);
  assert.equal(dryRun.status, 0, dryRun.stderr);
  assert.match(dryRun.stdout, /^dist$/m);
  assert.match(dryRun.stdout, /^packages\/.+\/dist$/m);

  const outside = runNode('remove-output.js', ['--dry-run', '..']);
  assert.notEqual(outside.status, 0);
  assert.match(outside.stderr, /Refusing to remove path outside the Node workspace/);
});

test('sample npm gate selects the PowerShell runner on Windows', () => {
  const result = runNode('run-samples-gate.js', ['--dry-run'], {
    ZLINK_TEST_PLATFORM: 'win32'
  });
  assert.equal(result.status, 0, result.stderr);
  const invocation = JSON.parse(result.stdout);
  assert.equal(invocation.command, 'powershell.exe');
  assert.ok(invocation.args.includes('-NonInteractive'));
  assert.ok(invocation.args.some((arg) => arg.endsWith(path.join('samples', 'run_samples.ps1'))));
  assert.ok(invocation.args.every((arg) => !arg.endsWith('.sh')));
});

test('sample npm gate preserves the shell runner on non-Windows hosts', () => {
  const result = runNode('run-samples-gate.js', ['--dry-run'], {
    ZLINK_TEST_PLATFORM: 'linux'
  });
  assert.equal(result.status, 0, result.stderr);
  const invocation = JSON.parse(result.stdout);
  assert.equal(invocation.command, 'bash');
  assert.ok(invocation.args.some((arg) => arg.endsWith(path.join('samples', 'run_samples.sh'))));
});

test('cross-language npm gate selects native commands on Windows', () => {
  const result = runNode('run-cross-language-smoke.js', ['--dry-run'], {
    ZLINK_TEST_PLATFORM: 'win32'
  });
  assert.equal(result.status, 0, result.stderr);
  const invocations = result.stdout.trim().split(/\r?\n/).map((line) => JSON.parse(line));
  assert.equal(invocations.length, 3);
  assert.equal(invocations[0].command, 'npm.cmd');
  assert.equal(invocations[1].command, 'dotnet');
  assert.equal(invocations[2].command, process.execPath);
  assert.ok(invocations.every(({ command, args }) => (
    command !== 'bash' && args.every((arg) => !arg.endsWith('.sh'))
  )));
});

test('cross-language npm gate preserves the native npm command on non-Windows hosts', () => {
  const result = runNode('run-cross-language-smoke.js', ['--dry-run'], {
    ZLINK_TEST_PLATFORM: 'linux'
  });
  assert.equal(result.status, 0, result.stderr);
  const [build] = result.stdout.trim().split(/\r?\n/).map((line) => JSON.parse(line));
  assert.equal(build.command, 'npm');
  assert.deepEqual(build.args, ['run', 'build']);
});

function runNode(script, args, extraEnv = {}) {
  return childProcess.spawnSync(process.execPath, [path.join(scriptsRoot, script), ...args], {
    cwd: nodeRoot,
    encoding: 'utf8',
    env: { ...process.env, ...extraEnv }
  });
}

async function importHttpPackageModule() {
  const moduleUrl = pathToFileURL(path.join(scriptsRoot, 'local-http-client-package.mjs'));
  return import(moduleUrl.href);
}

function temporaryRepo(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-node-local-package-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}

function writeNpmArchive(file, manifest) {
  const content = Buffer.from(JSON.stringify(manifest), 'utf8');
  const header = Buffer.alloc(512);
  writeTarString(header, 0, 100, 'package/package.json');
  writeTarOctal(header, 100, 8, 0o644);
  writeTarOctal(header, 108, 8, 0);
  writeTarOctal(header, 116, 8, 0);
  writeTarOctal(header, 124, 12, content.length);
  writeTarOctal(header, 136, 12, 0);
  header.fill(0x20, 148, 156);
  header[156] = '0'.charCodeAt(0);
  writeTarString(header, 257, 6, 'ustar');
  writeTarString(header, 263, 2, '00');
  const checksum = header.reduce((sum, value) => sum + value, 0);
  writeTarOctal(header, 148, 8, checksum);
  const padding = Buffer.alloc(Math.ceil(content.length / 512) * 512 - content.length);
  const tar = Buffer.concat([header, content, padding, Buffer.alloc(1024)]);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, zlib.gzipSync(tar));
}

function writeTarString(buffer, offset, length, value) {
  buffer.write(value, offset, Math.min(length, Buffer.byteLength(value)), 'utf8');
}

function writeTarOctal(buffer, offset, length, value) {
  const field = `${value.toString(8).padStart(length - 2, '0')}\0 `;
  buffer.write(field, offset, length, 'ascii');
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function readVersion(file, key) {
  const line = fs.readFileSync(file, 'utf8').split(/\r?\n/u).find((value) => value.startsWith(`${key}=`));
  assert.notEqual(line, undefined, `${key} must be defined in ${file}`);
  return line.slice(key.length + 1).trim();
}
