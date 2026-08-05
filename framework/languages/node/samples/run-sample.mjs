import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const samplesRoot = path.dirname(fileURLToPath(import.meta.url));
const nodeRoot = path.dirname(samplesRoot);
const definitionPath = process.argv[2];
if (!definitionPath) {
  throw new Error('Usage: node samples/run-sample.mjs <sample-runner.mjs>');
}
const runnerOptions = parseRunnerOptions(process.argv.slice(3));
const definition = await import(pathToFileURL(path.resolve(definitionPath)).href);
const { sampleName, runSample } = definition;
if (typeof sampleName !== 'string' || typeof runSample !== 'function') {
  throw new Error('The sample runner definition must export sampleName and runSample.');
}

const sampleRoot = path.join(samplesRoot, sampleName);
if (!fs.existsSync(path.join(sampleRoot, 'package.json'))) {
  throw new Error(`Unknown Node sample '${sampleName}'.`);
}

const runDir = fs.mkdtempSync(path.join(os.tmpdir(), `zlink-${sampleName.toLowerCase()}-`));
fs.chmodSync(runDir, 0o700);
const logDir = path.join(runDir, 'logs');
const workDir = path.join(runDir, 'work');
fs.mkdirSync(logDir, { recursive: true });
fs.mkdirSync(workDir, { recursive: true });

const children = [];
const reservedPorts = new Set();
let redisContainer;
let failed = false;
let cleaning = false;

async function main() {
  try {
    run('npm', ['run', 'build'], { cwd: sampleRoot });
    const redisEndpoint = await startRedis();
    const context = createContext(redisEndpoint);
    await runSample(context);
  } catch (error) {
    failed = true;
    printLogs();
    throw error;
  } finally {
    await cleanup();
    if (runnerOptions.keepRunDir || failed) {
      console.log(`runDir=${runDir}`);
    } else {
      fs.rmSync(runDir, { recursive: true, force: true });
    }
  }
}

function createContext(redisEndpoint) {
  const env = { ...process.env };
  return {
    env,
    logDir,
    nodeRoot,
    redisEndpoint,
    runDir,
    sampleRoot,
    workDir,
    port: reserveBrowserSafePort,
    writeConfig(name, sample) {
      const target = path.join(runDir, `${name}.json`);
      fs.writeFileSync(target, `${JSON.stringify({ sample }, null, 2)}\n`, { mode: 0o600 });
      fs.chmodSync(target, 0o600);
      return target;
    },
    async start(name, entry, args = [], extraEnv = {}) {
      const child = startNode(name, path.join(sampleRoot, entry), args, { ...env, ...extraEnv });
      children.push(child);
      return child;
    },
    async stop(name, signal = 'SIGINT') {
      const state = children.find((entry) => entry.name === name);
      if (!state) throw new Error(`Unknown sample process '${name}'.`);
      state.expectedStop = true;
      if (state.child.exitCode === null && state.child.signalCode === null) state.child.kill(signal);
      await waitForExit(state);
    },
    signal(name, signal = 'SIGINT') {
      const state = children.find((entry) => entry.name === name);
      if (!state) throw new Error(`Unknown sample process '${name}'.`);
      state.expectedStop = true;
      if (state.child.exitCode === null && state.child.signalCode === null) state.child.kill(signal);
    },
    waitTcp,
    waitHttp,
    waitLog,
    waitAnyLog,
    assertLogCount,
    runNode(entry, args = [], extraEnv = {}) {
      run(process.execPath, [entry, ...args], { cwd: sampleRoot, env: { ...env, ...extraEnv } });
    },
    runBrowser(definition, entryName) {
      const configPath = path.join(runDir, 'browser-runner.json');
      fs.writeFileSync(configPath, `${JSON.stringify(definition, null, 2)}\n`, { mode: 0o600 });
      fs.chmodSync(configPath, 0o600);
      const args = [
        path.join(nodeRoot, 'scripts/browser-e2e/run-sample.mjs'),
        sampleName,
        '--config',
        configPath
      ];
      if (entryName) args.push('--entry', entryName);
      run(process.execPath, args, { cwd: sampleRoot, env });
    }
  };
}

async function waitForExit(state) {
  const deadline = Date.now() + 10_000;
  while (state.status === undefined && Date.now() < deadline) await sleep(50);
  if (state.status === undefined) throw new Error(`${state.name} did not stop within 10 seconds.`);
}

async function reserveBrowserSafePort() {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    const port = 30000 + Math.floor(Math.random() * 10000);
    if (reservedPorts.has(port)) continue;
    if (await canBind(port)) {
      reservedPorts.add(port);
      return port;
    }
  }
  throw new Error('Unable to reserve a browser-safe loopback port.');
}

function canBind(port) {
  return new Promise((resolve) => {
    const server = net.createServer();
    server.once('error', () => resolve(false));
    server.listen(port, '127.0.0.1', () => server.close(() => resolve(true)));
  });
}

async function startRedis() {
  const name = `zlink-redis-node-sample-${process.pid}-${Date.now()}`;
  const image = runnerOptions.redisImage;
  const created = command('docker', [
    'create', '--name', name, '--tmpfs', '/data', '-p', '127.0.0.1::6379', image
  ]).trim();
  redisContainer = created;
  command('docker', ['start', created]);
  const portLine = command('docker', ['port', created, '6379/tcp']).trim();
  const port = portLine.slice(portLine.lastIndexOf(':') + 1);
  const endpoint = `127.0.0.1:${port}`;
  await waitTcp(`tcp://${endpoint}`);
  return endpoint;
}

function parseRunnerOptions(args) {
  const options = { keepRunDir: false, redisImage: 'redis:7.2-alpine' };
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === '--keep-run-dir') {
      options.keepRunDir = true;
      continue;
    }
    if (argument === '--redis-image') {
      const value = args[++index];
      if (!value || value.startsWith('--')) throw new Error('--redis-image <image> is required.');
      options.redisImage = value;
      continue;
    }
    throw new Error(`Unknown runner option '${argument}'.`);
  }
  return options;
}

function startNode(name, entry, args, env) {
  const logPath = path.join(logDir, `${name}.log`);
  const output = fs.openSync(logPath, 'a');
  const child = spawn(process.execPath, [entry, ...args], {
    cwd: sampleRoot,
    env,
    stdio: ['ignore', output, output]
  });
  fs.closeSync(output);
  const state = { child, logPath, name, status: undefined };
  child.once('exit', (code, signal) => {
    state.status = code ?? (signal ? 1 : 0);
  });
  return state;
}

async function waitTcp(endpoint) {
  const url = new URL(endpoint.replace(/^tcp:/, 'http:').replace(/^ws:/, 'http:'));
  await waitUntil(`endpoint ${endpoint}`, () => new Promise((resolve) => {
    const socket = net.connect(Number(url.port), url.hostname);
    socket.once('connect', () => { socket.destroy(); resolve(true); });
    socket.once('error', () => resolve(false));
    socket.setTimeout(500, () => { socket.destroy(); resolve(false); });
  }));
}

async function waitHttp(endpoint) {
  await waitUntil(`health ${endpoint}`, async () => {
    try {
      const response = await fetch(new URL('/health', endpoint), { signal: AbortSignal.timeout(1000) });
      return response.ok;
    } catch {
      return false;
    }
  });
}

async function waitLog(name, marker) {
  await waitUntil(`${name} log marker '${marker}'`, async () => {
    const target = path.join(logDir, `${name}.log`);
    return fs.existsSync(target) && fs.readFileSync(target, 'utf8').includes(marker);
  });
}

async function waitAnyLog(candidates) {
  await waitUntil(`one of ${candidates.map(({ name, marker }) => `${name} '${marker}'`).join(', ')}`, async () =>
    candidates.some(({ name, marker }) => {
      const target = path.join(logDir, `${name}.log`);
      return fs.existsSync(target) && fs.readFileSync(target, 'utf8').includes(marker);
    })
  );
}

function assertLogCount(name, marker, expected) {
  const target = path.join(logDir, `${name}.log`);
  const content = fs.existsSync(target) ? fs.readFileSync(target, 'utf8') : '';
  const actual = content.split(marker).length - 1;
  if (actual !== expected) {
    throw new Error(`${name} log marker '${marker}' count was ${actual}; expected ${expected}.`);
  }
}

async function waitUntil(description, probe) {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    ensureChildrenRunning();
    if (await probe()) return;
    await sleep(100);
  }
  throw new Error(`Timed out waiting for ${description}.`);
}

function ensureChildrenRunning() {
  const stopped = children.find((entry) => entry.status !== undefined && entry.expectedStop !== true);
  if (stopped) {
    throw new Error(`${stopped.name} exited before the sample client ran. See ${stopped.logPath}.`);
  }
}

function run(executable, args, options = {}) {
  const result = spawnSync(platformExecutable(executable), args, {
    cwd: options.cwd ?? sampleRoot,
    env: options.env ?? process.env,
    stdio: 'inherit'
  });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`${executable} ${args.join(' ')} exited with ${result.status}.`);
  }
}

function command(executable, args) {
  const result = spawnSync(platformExecutable(executable), args, { encoding: 'utf8' });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`${executable} ${args.join(' ')} failed: ${result.stderr.trim()}`);
  }
  return result.stdout;
}

function platformExecutable(executable) {
  return process.platform === 'win32' && executable === 'npm' ? 'npm.cmd' : executable;
}

async function cleanup() {
  if (cleaning) return;
  cleaning = true;
  for (const { child } of children.reverse()) {
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGINT');
  }
  await sleep(500);
  for (const { child } of children) {
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
  }
  if (redisContainer) {
    spawnSync(platformExecutable('docker'), ['rm', '-fv', redisContainer], { stdio: 'ignore' });
  }
}

function printLogs() {
  for (const entry of children) {
    if (!fs.existsSync(entry.logPath)) continue;
    const lines = fs.readFileSync(entry.logPath, 'utf8').trimEnd().split(/\r?\n/).slice(-80);
    process.stderr.write(`===== ${entry.name} =====\n${lines.join('\n')}\n`);
  }
}

function sleep(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.once(signal, () => {
    failed = true;
    void cleanup().finally(() => process.exit(signal === 'SIGINT' ? 130 : 143));
  });
}

await main();
