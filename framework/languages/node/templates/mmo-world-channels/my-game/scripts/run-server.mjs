import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import {
  closeSync,
  linkSync,
  mkdirSync,
  openSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync
} from 'node:fs';

const serverFile = '.logs/server.json';

function runningServer() {
  try {
    const state = JSON.parse(readFileSync(serverFile, 'utf8'));
    assert(Number.isSafeInteger(state.pid) && state.pid > 0, 'Invalid server PID');
    process.kill(state.pid, 0);
    return state;
  } catch (error) {
    if (error.code === 'ENOENT' || error.code === 'ESRCH') return undefined;
    throw error;
  }
}

function claimServer(logDir) {
  const pending = `${logDir}/starting.json`;
  writeFileSync(pending, JSON.stringify({ pid: process.pid }), { mode: 0o600 });
  try {
    linkSync(pending, serverFile);
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
    assert(!runningServer(), 'A server is already starting or running. Stop it with Ctrl+C.');
    // Only one launcher may reclaim a stale claim. Fresh claims are published atomically.
    const reclaim = '.logs/server-reclaim';
    mkdirSync(reclaim);
    try {
      assert(!runningServer(), 'A server is already starting or running. Stop it with Ctrl+C.');
      rmSync(serverFile, { force: true });
      linkSync(pending, serverFile);
    } finally {
      rmSync(reclaim, { recursive: true });
    }
  } finally {
    rmSync(pending);
  }
}

async function build(logDir, signal) {
  console.log('Preparing: building the server (Node.js)...');
  console.log(`Build log: ${logDir}/build.log`);
  const fd = openSync(`${logDir}/build.log`, 'w');
  try {
    for (const args of [['ci'], ['run', 'build']]) {
      if (signal?.aborted) return;
      const child = spawn('npm', args, {
        shell: process.platform === 'win32',
        stdio: ['ignore', fd, fd]
      });
      const code = await new Promise((resolve, reject) => {
        child.once('error', reject);
        child.once('close', resolve);
      });
      if (signal?.aborted) return;
      assert.equal(code, 0, `Node.js build failed. See ${logDir}/build.log`);
    }
  } finally {
    closeSync(fd);
  }
}

async function main() {
  const [mode, ...args] = process.argv.slice(2);
  if (mode === '--status') {
    if (runningServer()?.endpoints) process.exitCode = 0;
    else process.exitCode = 1;
    return;
  }
  if (mode === '--attach') {
    const state = runningServer();
    assert(state?.endpoints, 'Start the server with run-server and wait for its STREAM endpoint.');
    console.log('Using the running server (.logs/server.json)');
    const { attach } = await import('../dist/e2e/runner.js');
    await attach(args, state.endpoints);
    return;
  }
  assert((mode === '--serve' || mode === '--build') && !args.length, 'Usage: run-server');
  assert(!runningServer(), 'A server is already starting or running. Stop it with Ctrl+C.');
  const runId = randomUUID();
  const logDir = `.logs/run-${runId}`;
  mkdirSync(logDir, { recursive: true });
  if (mode === '--build') {
    await build(logDir);
    return;
  }
  claimServer(logDir);
  const abort = new AbortController();
  const publish = (state) => {
    const pending = `${logDir}/server.json`;
    writeFileSync(pending, JSON.stringify({ pid: process.pid, ...state }), { mode: 0o600 });
    renameSync(pending, serverFile);
  };
  const stop = () => {
    abort.abort();
    publish({});
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  try {
    await build(logDir, abort.signal);
    if (abort.signal.aborted) return;
    console.log('Build complete. Starting servers...');
    const { serve } = await import('../dist/e2e/runner.js');
    await serve(runId, logDir, abort.signal, (endpoints) => publish({ endpoints }));
  } finally {
    rmSync(serverFile, { force: true });
    process.off('SIGINT', stop);
    process.off('SIGTERM', stop);
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
