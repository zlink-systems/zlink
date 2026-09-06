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

let runDir;
let logDir;
let workDir;

const children = [];
const reservedPorts = new Set();
const portLeases = new Map();
const sharedPortLeaseDir = path.join(os.tmpdir(), 'zlink-sample-port-leases');
const redisPortRange = { min: 28000, max: 28099 };
const applicationPortRange = { min: 28100, max: 29999 };
const dockerCommandTimeoutMs = 10_000;
let redisContainer;
let failed = false;
let cleaning = false;
let stopSignal;
let interrupt;
const interrupted = new Promise((resolve) => { interrupt = resolve; });
const deferredOutput = [];

async function main() {
  const failures = [];
  let cleanupFailed = false;
  try {
    runDir = fs.mkdtempSync(path.join(os.tmpdir(), `zlink-${sampleName.toLowerCase()}-`));
    fs.chmodSync(runDir, 0o700);
    logDir = path.join(runDir, 'logs');
    workDir = path.join(runDir, 'work');
    fs.mkdirSync(logDir, { recursive: true, mode: 0o700 });
    fs.mkdirSync(workDir, { recursive: true });

    const execute = async () => {
      await run('npm', ['run', 'build'], { cwd: sampleRoot });
      assertRunning();
      const redisEndpoint = await startRedis();
      assertRunning();
      await runSample(createContext(redisEndpoint));
      ensureChildrenRunning();
    };
    const writeLine = console.log;
    console.log = (...args) => deferredOutput.push({ args, kind: 'log' });
    try {
      await Promise.race([execute(), interrupted.then(() => {
        throw new Error(`${sampleName} interrupted by ${stopSignal}.`);
      })]);
    } finally {
      console.log = writeLine;
    }
  } catch (error) {
    failed = true;
    failures.push(error);
  } finally {
    try {
      await cleanup();
    } catch (error) {
      failed = true;
      cleanupFailed = true;
      failures.push(error);
    }
    if (failures.length > 0 && !cleanupFailed) printLogs();
    if (runDir !== undefined) {
      if (runnerOptions.keepRunDir) {
        console.log(`runDir=${runDir}`);
      } else {
        try {
          if (failed && fs.existsSync(logDir)) {
            const evidenceDir = path.join(os.tmpdir(), 'zlink-sample-logs', path.basename(runDir));
            fs.mkdirSync(path.dirname(evidenceDir), { recursive: true, mode: 0o700 });
            fs.renameSync(logDir, evidenceDir);
            console.error(`log_dir=${evidenceDir}`);
          }
        } finally {
          fs.rmSync(runDir, { recursive: true, force: true });
        }
      }
    }
  }
  if (stopSignal !== undefined) {
    for (const error of failures) console.error(error);
    process.exit(stopSignal === 'SIGINT' ? 130 : 143);
  }
  if (failures.length === 1) throw failures[0];
  if (failures.length > 1) throw new AggregateError(failures, `${sampleName} failed during execution and cleanup.`);
  for (const entry of deferredOutput) {
    if (entry.kind === 'log') console.log(...entry.args);
    else process.stdout.write(entry.value);
  }
  console.log(`PASS ${sampleName}`);
}

function createContext(redisEndpoint) {
  const env = { ...process.env };
  const generatedSchemaRegistry = process.env.ZLINK_NODE_SCHEMA_REGISTRY_PATH
    ? path.resolve(process.env.ZLINK_NODE_SCHEMA_REGISTRY_PATH)
    : path.join(sampleRoot, 'dist', '.zlink-framework-json-schemas.cjs');
  if (process.env.ZLINK_NODE_PRELOAD_GENERATED_SCHEMAS !== '0' && fs.existsSync(generatedSchemaRegistry)) {
    const preload = `--require=${generatedSchemaRegistry}`;
    env.NODE_OPTIONS = `${env.NODE_OPTIONS ?? ''} ${preload}`.trim();
  }
  return {
    env,
    lane: runnerOptions.lane,
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
      return startNode(name, path.join(sampleRoot, entry), args, { ...env, ...extraEnv });
    },
    startCommand(...args) { return startCommand(...args); },
    runCommand(...args) { return run(...args); },
    async stop(name, signal = 'SIGINT') {
      const state = children.find((entry) => entry.name === name);
      if (!state) throw new Error(`Unknown sample process '${name}'.`);
      state.expectedStop = true;
      signalChild(state, signal);
      await waitForExit(state);
      if (signal === 'SIGKILL' ? state.signalCode !== signal : state.exitCode !== 0 && state.signalCode !== signal) {
        throw new Error(`${name} stopped with ${state.signalCode ?? state.exitCode}, expected ${signal}.`);
      }
    },
    waitTcp,
    waitHttp,
    waitLog,
    waitAnyLog,
    assertLogCount,
    async runNode(entry, args = [], extraEnv = {}) {
      await run(process.execPath, [entry, ...args], { cwd: sampleRoot, env: { ...env, ...extraEnv } });
    },
    async runBrowser(definition, entryName) {
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
      //  Tee the browser scenario's output into browser-client.log as well as this runner's
      //  stdout. The client's completion markers are only observable there, and a sample runner
      //  must confirm them from a log rather than trusting the browser's own verdict.
      const browserLog = path.join(logDir, 'browser-client.log');
      const state = startCommand('browser-client', process.execPath, args, {
        cwd: sampleRoot,
        env,
        expectedStop: true
      });
      await state.exited;
      if (state.error) throw state.error;
      const captured = fs.readFileSync(browserLog, 'utf8');
      deferredOutput.push({ kind: 'write', value: captured });
      if (state.status !== 0) {
        throw new Error(`browser sample exited with ${state.status}. See ${browserLog}.`);
      }
    },
    startBrowser(definition, entryName) {
      const completionSignalPath = path.join(runDir, 'browser-lifecycle-complete');
      const configPath = path.join(runDir, 'browser-runner.json');
      fs.writeFileSync(configPath, `${JSON.stringify({
        ...definition,
        completionSignalPath
      }, null, 2)}\n`, { mode: 0o600 });
      fs.chmodSync(configPath, 0o600);
      const args = [
        sampleName,
        '--config',
        configPath
      ];
      if (entryName) args.push('--entry', entryName);
      const state = startNode(
        'browser-client',
        path.join(nodeRoot, 'scripts/browser-e2e/run-sample.mjs'),
        args,
        env
      );
      return {
        async complete() {
          if (state.status !== undefined) {
            throw new Error(`Browser sample exited before lifecycle evidence completed. See ${state.logPath}.`);
          }
          state.expectedStop = true;
          fs.writeFileSync(completionSignalPath, 'complete\n', { mode: 0o600 });
          await waitForExit(state);
          if (state.status !== 0) {
            throw new Error(`Browser sample exited with ${state.status}. See ${state.logPath}.`);
          }
        }
      };
    }
  };
}

async function waitForExit(state) {
  const deadline = Date.now() + 10_000;
  while (state.status === undefined && Date.now() < deadline) await sleep(50);
  if (state.status === undefined) throw new Error(`${state.name} did not stop within 10 seconds.`);
  await state.exited;
}

function signalChild(state, signal) {
  if (state.closed || state.child.pid === undefined) return false;
  if (!state.processGroup) return state.child.kill(signal);
  try {
    process.kill(-state.child.pid, signal);
    return true;
  } catch (error) {
    if (error.code !== 'ESRCH') throw error;
    return false;
  }
}

async function reserveBrowserSafePort() {
  return reserveLeasedPort(applicationPortRange, 'application');
}

async function reserveLeasedPort(range, purpose) {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    assertRunning();
    const port = range.min + Math.floor(Math.random() * (range.max - range.min + 1));
    if (reservedPorts.has(port)) continue;
    const leasePath = acquirePortLease(port);
    if (leasePath === undefined) continue;
    const available = await canBind(port);
    if (stopSignal !== undefined || cleaning) {
      fs.rmSync(leasePath, { force: true });
      assertRunning();
    }
    if (available) {
      reservedPorts.add(port);
      portLeases.set(port, leasePath);
      return port;
    }
    fs.rmSync(leasePath, { force: true });
  }
  throw new Error(`Unable to reserve a ${purpose} loopback port.`);
}

function acquirePortLease(port) {
  fs.mkdirSync(sharedPortLeaseDir, { recursive: true, mode: 0o700 });
  const leasePath = path.join(sharedPortLeaseDir, `${port}.lock`);
  for (let attempt = 0; attempt < 2; attempt += 1) {
    try {
      const descriptor = fs.openSync(leasePath, 'wx', 0o600);
      fs.writeFileSync(descriptor, `${process.pid}\n`);
      fs.closeSync(descriptor);
      return leasePath;
    } catch (error) {
      if (error?.code !== 'EEXIST') throw error;
      let owner;
      try {
        owner = Number.parseInt(fs.readFileSync(leasePath, 'utf8'), 10);
      } catch (readError) {
        if (readError?.code === 'ENOENT') continue;
        throw readError;
      }
      if (Number.isInteger(owner) && isProcessRunning(owner)) return undefined;
      fs.rmSync(leasePath, { force: true });
    }
  }
  return undefined;
}

function releasePortLease(port) {
  const leasePath = portLeases.get(port);
  if (leasePath !== undefined) fs.rmSync(leasePath, { force: true });
  portLeases.delete(port);
  reservedPorts.delete(port);
}

function isProcessRunning(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error?.code === 'EPERM';
  }
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
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const port = await reserveLeasedPort(redisPortRange, 'Redis');
    try {
      const created = command('docker', [
        'create', '--name', name, '--tmpfs', '/data', '-p', `127.0.0.1:${port}:6379`, image
      ]).trim();
      if (!/^[0-9a-f]{12,64}$/.test(created)) {
        throw new Error(`Docker create returned an invalid Redis container id for ${name}.`);
      }
      redisContainer = created;
      command('docker', ['start', created]);
      const running = command('docker', [
        'inspect', '-f', '{{.State.Running}}', created
      ]).trim();
      const publishedPort = command('docker', [
        'inspect',
        '-f',
        '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}',
        created
      ]).trim();
      if (running !== 'true' || publishedPort !== String(port)) {
        const mismatchedContainer = redisContainer;
        redisContainer = undefined;
        removeRedisAttempt(mismatchedContainer, name);
        releasePortLease(port);
        continue;
      }
      const endpoint = `127.0.0.1:${port}`;
      await waitTcp(`tcp://${endpoint}`);
      return endpoint;
    } catch (error) {
      releasePortLease(port);
      removeRedisAttempt(redisContainer, name);
      redisContainer = undefined;
      if (!/address already in use|port is already allocated|Bind for .* failed/i.test(String(error))) {
        throw error;
      }
    }
  }
  throw new Error('Unable to reserve and bind a Redis loopback port.');
}

function removeRedisAttempt(containerId, name) {
  let exactId = containerId;
  if (!/^[0-9a-f]{12,64}$/.test(exactId ?? '')) {
    const inspected = spawnSync(platformExecutable('docker'), [
      'inspect', '--type', 'container', '-f', '{{.Id}}', name
    ], { encoding: 'utf8', timeout: dockerCommandTimeoutMs });
    if (inspected.status === 0) exactId = inspected.stdout.trim();
  }
  if (!/^[0-9a-f]{12,64}$/.test(exactId ?? '')) return;
  spawnSync(platformExecutable('docker'), ['rm', '-fv', exactId], {
    stdio: 'ignore', timeout: dockerCommandTimeoutMs
  });
}

function parseRunnerOptions(args) {
  const options = { keepRunDir: false, redisImage: 'redis:7.2-alpine', lane: undefined };
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === '--keep-run-dir') {
      options.keepRunDir = true;
      continue;
    }
    if (argument === '--lane') {
      const value = args[++index];
      if (!value || value.startsWith('--')) throw new Error('--lane <name> is required.');
      options.lane = value;
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
  return startCommand(name, process.execPath, [entry, ...args], { env });
}

function assertRunning() {
  if (stopSignal !== undefined || cleaning) throw new Error(`${sampleName} is stopping.`);
}

function startCommand(name, command, args, options = {}) {
  assertRunning();
  const logPath = path.join(logDir, `${name}.log`);
  const output = fs.openSync(logPath, 'a');
  let child;
  try {
    child = spawn(platformExecutable(command), args, {
      cwd: options.cwd ?? sampleRoot,
      env: options.env ?? process.env,
      detached: process.platform !== 'win32',
      // Descendants inherit these pipes. 'close' waits for their output handles too.
      stdio: ['ignore', 'pipe', 'pipe']
    });
  } catch (error) {
    fs.closeSync(output);
    throw error;
  }
  const state = { child, exitCode: undefined, logPath, name, signalCode: undefined,
    status: undefined, expectedStop: options.expectedStop ?? false,
    processGroup: process.platform !== 'win32', closed: false };
  children.push(state);
  for (const [stream, inherited] of [[child.stdout, process.stdout], [child.stderr, process.stderr]]) {
    stream.on('data', (chunk) => {
      fs.writeSync(output, chunk);
      if (options.inheritOutput) inherited.write(chunk);
    });
  }
  child.once('error', (error) => { state.error = error; });
  child.once('exit', (code, signal) => {
    state.exitCode = code;
    state.signalCode = signal;
    state.status = code ?? (signal ? 1 : 0);
  });
  state.exited = new Promise((resolve) => child.once('close', () => {
    state.closed = true;
    fs.closeSync(output);
    resolve(state.status);
  }));
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
  const failedStart = children.find((entry) => entry.error !== undefined);
  if (failedStart) throw failedStart.error;
  const stopped = children.find((entry) => entry.status !== undefined && entry.expectedStop !== true);
  if (stopped) {
    throw new Error(`${stopped.name} exited before the sample client ran. See ${stopped.logPath}.`);
  }
}

function run(executable, args, options = {}) {
  const state = startCommand(`command-${children.length}`, executable, args, {
    ...options,
    expectedStop: true,
    inheritOutput: true
  });
  return state.exited.then(() => {
    if (state.error) throw state.error;
    if (state.status !== 0) {
      throw new Error(`${executable} ${args.join(' ')} exited with ${state.status}.`);
    }
  });
}

function command(executable, args) {
  const result = spawnSync(platformExecutable(executable), args, {
    encoding: 'utf8',
    timeout: executable === 'docker' ? dockerCommandTimeoutMs : undefined
  });
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
  if (cleaning) return cleaning;
  cleaning = cleanChildren();
  return cleaning;
}

async function cleanChildren() {
  const teardownFailures = new Map();
  const active = children.filter((state) => !state.closed);
  const exited = active.map((state) => state.exited);
  for (const state of [...active].reverse()) {
    signalChild(state, 'SIGTERM');
  }
  // Keep the existing grace deadline; finish as soon as the owned processes close.
  let deadline;
  try {
    await Promise.race([Promise.all(exited), new Promise((resolve) => {
      deadline = setTimeout(resolve, 500);
    })]);
  } finally {
    clearTimeout(deadline);
  }
  for (const state of active) {
    if (state.exitCode === 137 || state.exitCode === -9 || state.signalCode === 'SIGKILL') {
      teardownFailures.set(state, state.signalCode ?? state.exitCode);
    }
    if (!state.closed) {
      if (signalChild(state, 'SIGKILL')) teardownFailures.set(state, 'SIGKILL');
    }
  }
  await Promise.all(exited);
  if (redisContainer) {
    removeRedisAttempt(redisContainer, '');
  }
  for (const leasePath of portLeases.values()) fs.rmSync(leasePath, { force: true });
  portLeases.clear();
  if (teardownFailures.size > 0) {
    throw new Error([...teardownFailures]
      .map(([state, status]) => `Sample role ${state.name} exited during cleanup with status ${status}.`)
      .join('\n'));
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
  process.on(signal, () => {
    stopSignal ??= signal;
    interrupt();
  });
}

await main();
