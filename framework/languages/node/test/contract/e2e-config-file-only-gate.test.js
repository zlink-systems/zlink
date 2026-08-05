import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../..');
const excluded = new Set(['dist', 'log', 'logs', 'node_modules']);

function sourceFiles(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    if (excluded.has(entry.name)) {
      return [];
    }
    const target = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      return sourceFiles(target);
    }
    return /\.(?:cjs|js|mjs|ts)$/.test(entry.name) ? [target] : [];
  });
}

test('Node sample and e2e application code does not read environment configuration', () => {
  const applicationFiles = [
    ...sourceFiles(path.join(root, 'e2e')),
    ...sourceFiles(path.join(root, 'samples')).filter((file) =>
      !file.includes(`${path.sep}Runner${path.sep}`) && path.basename(file) !== 'run-sample.mjs')
  ];
  const offenders = applicationFiles
    .filter((file) => /\bprocess\.env\b/.test(fs.readFileSync(file, 'utf8')))
    .map((file) => path.relative(root, file));

  assert.deepEqual(offenders, []);
});

test('Node configuration modules disable environment providers and reject extra host arguments', () => {
  const applicationRoots = [path.join(root, 'samples'), path.join(root, 'e2e')];
  const modules = applicationRoots
    .flatMap(sourceFiles)
    .filter((file) => /ConfigModule\.forRoot\(/.test(fs.readFileSync(file, 'utf8')));
  const violations = [];

  for (const file of modules) {
    const source = fs.readFileSync(file, 'utf8');
    for (const contract of [
      ['ignoreEnvFile', /ignoreEnvFile:\s*true/],
      ['skipProcessEnv', /skipProcessEnv:\s*true/],
      ['typed provider', /inject:\s*\[ConfigService\]/],
      ['config-only arguments', /args\.length\s*!==\s*2/]
    ]) {
      if (!contract[1].test(source)) violations.push(`${path.relative(root, file)}:${contract[0]}`);
    }
  }

  assert.deepEqual(violations, []);
});

test('Node framework factories receive validated configuration through injection', () => {
  const factories = [path.join(root, 'samples'), path.join(root, 'e2e')]
    .flatMap(sourceFiles)
    .filter((file) => /ZLinkModule\.forRootFactory\(/.test(fs.readFileSync(file, 'utf8')));
  const offenders = factories
    .filter((file) => !/inject:\s*\[[^\]]+\]/s.test(fs.readFileSync(file, 'utf8')))
    .map((file) => path.relative(root, file));

  assert.deepEqual(offenders, []);
});

test('AutomaticTurnDispatch reads each host configuration once at its Configuration boundary', () => {
  const rootDirectory = path.join(root, 'e2e/AutomaticTurnDispatch');
  const playFactory = fs.readFileSync(path.join(rootDirectory, 'Server/Play/play-host-factory.ts'), 'utf8');
  const externalApiEntry = fs.readFileSync(path.join(rootDirectory, 'Server/ExternalApi/main.ts'), 'utf8');

  assert.doesNotMatch(playFactory, /readAutomaticTurnConfiguration/);
  assert.match(playFactory, /createAutomaticTurnConfiguration\(/);
  assert.doesNotMatch(externalApiEntry, /function readConfig\(/);
  assert.match(externalApiEntry, /Configuration\/external-api-options/);
});

test('every Node e2e framework host receives only one role configuration file path', () => {
  const e2eRoot = path.join(root, 'e2e');
  const runners = fs.readdirSync(e2eRoot, { withFileTypes: true })
    .filter((entry) => entry.isDirectory() && fs.existsSync(path.join(e2eRoot, entry.name, 'Server')))
    .map((entry) => path.join(e2eRoot, entry.name, 'run_e2e.sh'))
    .filter((file) => fs.existsSync(file));
  const missingLaunches = [];
  const violations = [];

  for (const runner of runners) {
    const commands = fs.readFileSync(runner, 'utf8').replace(/\\\n/g, ' ')
      .split('\n')
      .map((line) => line.trim())
      .filter((line) => /^start_server\s+/.test(line) || /^node\s+"\$[A-Z0-9_]*MAIN"/.test(line));
    if (commands.length === 0) missingLaunches.push(path.relative(root, runner));
    for (const command of commands) {
      if (!/--config\s+/.test(command)) {
        violations.push(`${path.relative(root, runner)}: ${command}`);
      }
    }
  }

  assert.deepEqual(missingLaunches, []);
  assert.deepEqual(violations, []);
});

test('Node sample runners do not dispatch sample-specific behavior by sample name', () => {
  const sharedRunner = fs.readFileSync(path.join(root, 'samples/run-sample.mjs'), 'utf8');
  const sampleDirectories = fs.readdirSync(path.join(root, 'samples'), { withFileTypes: true })
    .filter((entry) => entry.isDirectory() && entry.name.endsWith('.Ts'))
    .map((entry) => path.join(root, 'samples', entry.name));
  const wrappers = sampleDirectories.flatMap((directory) =>
    ['run_sample.sh', 'run_sample.ps1']
      .map((name) => path.join(directory, name))
      .filter((file) => fs.existsSync(file)));

  assert.doesNotMatch(sharedRunner, /sampleDefinitions\s*\[/);
  assert.doesNotMatch(sharedRunner, /['"](?:Bingo|TicTacToe|SupportChat|DeliveryDispatch|GameQuest|ShoppingMall)\.Ts['"]\s*:/);
  for (const wrapper of wrappers) {
    assert.doesNotMatch(fs.readFileSync(wrapper, 'utf8'), /run-sample\.mjs['"]?\s+[A-Za-z]+\.Ts/);
    assert.match(fs.readFileSync(wrapper, 'utf8'), /Runner[\\/]sample-runner\.mjs/);
  }
});

test('Node topology sample runners write one configuration per server role', () => {
  for (const sample of ['DeliveryDispatch.Ts', 'GameQuest.Ts', 'ShoppingMall.Ts', 'TicTacToe.Ts']) {
    const runner = fs.readFileSync(path.join(root, 'samples', sample, 'Runner/sample-runner.mjs'), 'utf8');
    assert.match(runner, /roleConfig\(/, sample);
    assert.doesNotMatch(runner, /const configPath\s*=\s*ctx\.writeConfig/, sample);
  }
});

test('Node clients that consume topology or file paths use typed configuration files', () => {
  const nodeClients = [
    'DiscoveryRegistryHa/Client/Support/client-options.ts',
    'PubSub/Client/Support/client-options.ts',
    'RegistrationCodec/Client/Support/client-options.ts',
    'RegistryMessaging/Client/Support/client-options.ts',
    'ResilienceLifecycle/Client/Support/client-options.ts',
    'RuntimeMonitoring/Client/Support/client-options.ts'
  ];
  const browserClients = [
    'AutomaticTurnDispatch/Client/main.ts',
    'ObservabilityOps/Client/Support/scenario-support.ts',
    'SpotActorTransfer/Client/Support/scenario-support.ts',
    'SpotService/Client/main.ts',
    'ToActorMessaging/Client/main.ts'
  ];
  const violations = [];
  for (const client of nodeClients) {
    const source = fs.readFileSync(path.join(root, 'e2e', client), 'utf8');
    if (!/args\.length\s*!==\s*2/.test(source) || !/args\[0\]\s*!==\s*['"]--config['"]/.test(source)) {
      violations.push(client);
    }
  }
  for (const client of browserClients) {
    const source = fs.readFileSync(path.join(root, 'e2e', client), 'utf8');
    if (!/browserE2eConfig/.test(source) || /node:fs/.test(source)) violations.push(client);
  }
  const browserRunner = fs.readFileSync(path.join(root, 'scripts/browser-e2e/run-e2e-client.mjs'), 'utf8');
  if (!/url\.pathname === ['"]\/config\.json['"]/.test(browserRunner)) violations.push('scripts/browser-e2e/run-e2e-client.mjs:/config.json');
  assert.deepEqual(violations, []);
});

test('Node runners protect and remove generated configuration files', () => {
  const e2eRoot = path.join(root, 'e2e');
  const writers = sourceFiles(e2eRoot)
    .filter((file) => path.basename(file) === 'write-config.mjs');
  const writerViolations = writers
    .filter((file) => !/mode:\s*0o600/.test(fs.readFileSync(file, 'utf8')))
    .map((file) => path.relative(root, file));
  const runnerViolations = [];

  for (const writer of writers) {
    const runner = path.join(path.dirname(writer), 'run_e2e.sh');
    if (!fs.existsSync(runner)) continue;
    const source = fs.readFileSync(runner, 'utf8');
    if (!/CONFIG_DIR="\$\(mktemp -d\)"/.test(source)) runnerViolations.push(`${path.relative(root, runner)}:temporary directory`);
    if (!/chmod 700 "\$CONFIG_DIR"/.test(source)) runnerViolations.push(`${path.relative(root, runner)}:directory mode`);
    if (!/rm -rf "\$CONFIG_DIR"/.test(source)) runnerViolations.push(`${path.relative(root, runner)}:cleanup`);
    if (/\$LOG_DIR\/[^"]*config\.json/.test(source)) runnerViolations.push(`${path.relative(root, runner)}:persistent configuration`);
  }

  assert.deepEqual(writerViolations, []);
  assert.deepEqual(runnerViolations, []);
});
