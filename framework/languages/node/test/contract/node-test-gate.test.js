const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const { once } = require('node:events');

const workspaceRoot = path.resolve(__dirname, '..', '..');
const lockModule = path.join(workspaceRoot, 'scripts', 'node-test-gate-lock.js');

test('runtime gate leaves actual browser E2E to the dedicated browser gate', () => {
  const source = fs.readFileSync(path.join(workspaceRoot, 'scripts', 'run_node_runtime_gate.js'), 'utf8');
  const commands = [];
  vm.runInNewContext(source, {
    __dirname: path.join(workspaceRoot, 'scripts'),
    require(name) {
      if (name === './node-test-gate-lock') return { acquireNodeTestGateLock: () => () => {} };
      if (name === 'node:child_process') return {
        spawnSync(command, args) {
          commands.push([command, ...args]);
          return { status: 0, stdout: '# Subtest: fixture\nok 1 - fixture\n1..1\n# tests 1\n' };
        }
      };
      return require(name);
    },
    process: {
      env: {}, versions: process.versions, platform: process.platform, execPath: process.execPath,
      stdout: { write() {} }, stderr: { write() {} },
      exit(code) { throw new Error(`Unexpected gate exit ${code}`); }
    },
    console: { log() {}, error() {} }
  });
  const testFiles = commands.filter((args) => args.includes('--test')).map((args) => args.at(-1));
  assert(testFiles.includes(__filename));
  assert(!testFiles.some((file) => file.startsWith(path.join(workspaceRoot, 'test', 'browser') + path.sep)));
  assert(!testFiles.some((file) => /^(sample-|tictactoe-|node-sample-client-bundle)/.test(path.basename(file))));
  const manifest = JSON.parse(fs.readFileSync(path.join(workspaceRoot, 'package.json'), 'utf8'));
  assert.equal(manifest.scripts['test:browser'], 'node --test test/browser/*.test.js');
  assert(!manifest.workspaces.some((entry) => entry.startsWith('samples/')));
});

test('standalone samples stay outside workspaces and carry package-mode build inputs', () => {
  const sampleNames = [
    'Bingo.Ts',
    'DeliveryDispatch.Ts',
    'GameQuest.Ts',
    'ShoppingMall.Ts',
    'SupportChat.Ts',
    'TicTacToe.Ts',
    'ZoneWorld'
  ];
  const generator = fs.readFileSync(
    path.join(workspaceRoot, 'scripts', 'generate-framework-json-schemas.mjs'),
    'utf8'
  );
  const sampleRunner = fs.readFileSync(path.join(workspaceRoot, 'samples', 'run-sample.mjs'), 'utf8');
  const browserRunner = fs.readFileSync(
    path.join(workspaceRoot, 'scripts', 'browser-e2e', 'run-sample.mjs'),
    'utf8'
  );
  // Samples pin the published framework version; the workspace manifest is the
  // sync-version.py registry entry that carries the same number.
  const frameworkVersion = JSON.parse(
    fs.readFileSync(path.join(workspaceRoot, 'packages', 'framework', 'package.json'), 'utf8')
  ).version;
  const browserSamples = new Set([
    'Bingo.Ts', 'DeliveryDispatch.Ts', 'GameQuest.Ts', 'SupportChat.Ts', 'TicTacToe.Ts'
  ]);

  for (const sampleName of sampleNames) {
    const sampleRoot = path.join(workspaceRoot, 'samples', sampleName);
    const manifest = JSON.parse(fs.readFileSync(path.join(sampleRoot, 'package.json'), 'utf8'));
    const tsconfig = JSON.parse(fs.readFileSync(path.join(sampleRoot, 'tsconfig.json'), 'utf8'));
    assert.equal(manifest.dependencies['@zlink-systems/framework'], frameworkVersion, sampleName);
    assert.equal(manifest.scripts.prebuild, 'node scripts/prepare-dependencies.mjs', sampleName);
    assert.equal(manifest.scripts.sample, 'node scripts/run-sample.mjs Runner/sample-runner.mjs', sampleName);
    assert.doesNotMatch(manifest.scripts.build, /\.\.\/\.\.\/(?:node_modules|scripts)/, sampleName);
    assert.equal(tsconfig.extends, undefined, sampleName);
    assert.equal(tsconfig.compilerOptions.paths, undefined, sampleName);
    assert.equal(tsconfig.compilerOptions.moduleResolution, 'Node16', sampleName);
    assert.equal(fs.existsSync(path.join(sampleRoot, 'package-lock.json')), false, sampleName);
    assert.equal(
      fs.readFileSync(path.join(sampleRoot, 'scripts', 'generate-framework-json-schemas.mjs'), 'utf8'),
      generator,
      sampleName
    );
    assert.equal(
      fs.readFileSync(path.join(sampleRoot, 'scripts', 'run-sample.mjs'), 'utf8'),
      sampleRunner,
      sampleName
    );
    if (browserSamples.has(sampleName)) {
      assert.equal(
        fs.readFileSync(path.join(sampleRoot, 'scripts', 'browser-e2e', 'run-sample.mjs'), 'utf8'),
        browserRunner,
        sampleName
      );
    }
  }
});

test('node runtime and coverage gates isolate native test handles and concurrent runs', () => {
  for (const scriptName of ['run_node_runtime_gate.js', 'run_node_coverage_gate.js']) {
    const source = fs.readFileSync(path.join(workspaceRoot, 'scripts', scriptName), 'utf8');
    assert.match(source, /acquireNodeTestGateLock/);
  }

  const runtimeGate = fs.readFileSync(path.join(workspaceRoot, 'scripts', 'run_node_runtime_gate.js'), 'utf8');
  assert.doesNotMatch(runtimeGate, /--test-force-exit/);
  assert.match(runtimeGate, /timeout: 600000/);
  assert.match(runtimeGate, /plan=.*announced=/);
  assert.match(runtimeGate, /plan=.*completed=/);
  assert.match(runtimeGate, /completedTestCount !== expectedTestCount/);
});

test('node test gate lock rejects a live owner and releases after owner exit', async () => {
  const lockRoot = path.join(workspaceRoot, `.lock-contract-${process.pid}-${Date.now()}`);
  const holder = childProcess.spawn(process.execPath, [
    '-e',
    lockScript('holder', "process.stdout.write('ready\\n'); setInterval(() => {}, 1000);")
  ], {
    cwd: workspaceRoot,
    stdio: ['ignore', 'pipe', 'pipe']
  });

  try {
    await once(holder.stdout, 'data');
    const blocked = runLockProcess(lockRoot, 'contender');
    assert.notEqual(blocked.status, 0);
    assert.match(blocked.stderr, /already running/);
  } finally {
    holder.kill('SIGTERM');
    await once(holder, 'exit');
  }

  const reacquired = runLockProcess(lockRoot, 'successor');
  assert.equal(reacquired.status, 0, reacquired.stderr);

  function lockScript(gateName, afterAcquire = '') {
    return [
      `const { acquireNodeTestGateLock } = require(${JSON.stringify(lockModule)});`,
      `const release = acquireNodeTestGateLock(${JSON.stringify(lockRoot)}, ${JSON.stringify(gateName)});`,
      afterAcquire,
      afterAcquire.length === 0 ? 'release();' : ''
    ].join('\n');
  }

  function runLockProcess(root, gateName) {
    return childProcess.spawnSync(process.execPath, [
      '-e',
      [
        `const { acquireNodeTestGateLock } = require(${JSON.stringify(lockModule)});`,
        `const release = acquireNodeTestGateLock(${JSON.stringify(root)}, ${JSON.stringify(gateName)});`,
        'release();'
      ].join('\n')
    ], {
      cwd: workspaceRoot,
      encoding: 'utf8'
    });
  }
});
