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

test('Node sample application code does not read environment configuration', () => {
  const applicationFiles = [
    ...sourceFiles(path.join(root, 'samples')).filter((file) =>
      !file.includes(`${path.sep}Runner${path.sep}`)
      && !file.includes(`${path.sep}scripts${path.sep}`)
      && path.basename(file) !== 'run-sample.mjs')
  ];
  const offenders = applicationFiles
    .filter((file) => /\bprocess\.env\b/.test(fs.readFileSync(file, 'utf8')))
    .map((file) => path.relative(root, file));

  assert.deepEqual(offenders, []);
});

test('Node configuration modules disable environment providers and reject extra host arguments', () => {
  const applicationRoots = [path.join(root, 'samples')];
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
  const factories = [path.join(root, 'samples')]
    .flatMap(sourceFiles)
    .filter((file) => /ZLinkModule\.forRootFactory\(/.test(fs.readFileSync(file, 'utf8')));
  const offenders = factories
    .filter((file) => !/inject:\s*\[[^\]]+\]/s.test(fs.readFileSync(file, 'utf8')))
    .map((file) => path.relative(root, file));

  assert.deepEqual(offenders, []);
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
