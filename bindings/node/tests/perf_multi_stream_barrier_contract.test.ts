// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { PassThrough } = require('node:stream');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {
  attachProcessCapture,
  buildClientSpawn,
  coordinateRunnerStart,
  spawnMultiPair
} = require('../perf/multi/perf_multi_orchestrator');
const { resolveMultiConnectReadyTimeoutMs } = require('../perf/common/perf_args');
const {
  createStreamControlBarrier
} = require('../perf/multi/perf_multi_stream_server');
const {
  resolveMultiStreamClientCount
} = require('../perf/multi/perf_multi_common');

function fakeManagedProcess() {
  const child = new EventEmitter();
  child.stdout = new PassThrough();
  child.stderr = new PassThrough();
  child.stdin = new PassThrough();
  child.exitCode = null;
  child.signalCode = null;
  attachProcessCapture(child, []);
  return child;
}

test('STREAM connection timeout follows the documented default and environment', () => {
  const previous = process.env.PERF_MULTI_CONNECT_READY_TIMEOUT_MS;
  const fallback = process.env.PERF_CONNECT_READY_TIMEOUT_MS;
  try {
    delete process.env.PERF_MULTI_CONNECT_READY_TIMEOUT_MS;
    delete process.env.PERF_CONNECT_READY_TIMEOUT_MS;
    assert.equal(resolveMultiConnectReadyTimeoutMs(undefined), 10000);
    process.env.PERF_CONNECT_READY_TIMEOUT_MS = '7000';
    assert.equal(resolveMultiConnectReadyTimeoutMs(undefined), 7000);
    process.env.PERF_MULTI_CONNECT_READY_TIMEOUT_MS = '8000';
    assert.equal(resolveMultiConnectReadyTimeoutMs(undefined), 8000);
    assert.equal(resolveMultiConnectReadyTimeoutMs(9000), 9000);
  } finally {
    if (previous === undefined) delete process.env.PERF_MULTI_CONNECT_READY_TIMEOUT_MS;
    else process.env.PERF_MULTI_CONNECT_READY_TIMEOUT_MS = previous;
    if (fallback === undefined) delete process.env.PERF_CONNECT_READY_TIMEOUT_MS;
    else process.env.PERF_CONNECT_READY_TIMEOUT_MS = fallback;
  }
});

test('STREAM client preparation failure reaps its already-ready server', async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-stream-ready-'));
  const pidFile = path.join(directory, 'server.pid');
  const oldPidFile = process.env.PERF_TEST_SERVER_PID_FILE;
  const oldClient = process.env.PERF_STREAM_CLIENT_BINARY;
  const originalExistsSync = fs.existsSync;
  process.env.PERF_TEST_SERVER_PID_FILE = pidFile;
  delete process.env.PERF_STREAM_CLIENT_BINARY;
  fs.existsSync = (candidate) => String(candidate).endsWith('/bindings/c/build/perf/perf_stream_client')
    ? false : originalExistsSync(candidate);
  let serverPid;
  try {
    await assert.rejects(spawnMultiPair(
      '../../../tests/fixtures/perf_multi_ready_server.js', null,
      { pattern: 'STREAM', transport: 'tcp', msgSize: 1024, duration: 1,
        clients: 1, pinCpu: false, serverReadyTimeoutMs: 1000 }
    ), /shared perf_stream_client not found/);
    serverPid = Number(fs.readFileSync(pidFile, 'utf8'));
    assert.throws(() => process.kill(serverPid, 0), { code: 'ESRCH' });
  } finally {
    if (serverPid) {
      try { process.kill(serverPid, 'SIGKILL'); } catch (_) {}
    }
    if (oldPidFile === undefined) delete process.env.PERF_TEST_SERVER_PID_FILE;
    else process.env.PERF_TEST_SERVER_PID_FILE = oldPidFile;
    if (oldClient === undefined) delete process.env.PERF_STREAM_CLIENT_BINARY;
    else process.env.PERF_STREAM_CLIENT_BINARY = oldClient;
    fs.existsSync = originalExistsSync;
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test('shared STREAM client is spawned behind the START gate', () => {
  const previous = process.env.PERF_STREAM_CLIENT_BINARY;
  process.env.PERF_STREAM_CLIENT_BINARY = '/tmp/perf_stream_client';
  try {
    const spawn = buildClientSpawn(null, ['--endpoint', 'tcp://127.0.0.1:5555'], {
      pattern: 'STREAM',
      transport: 'tcp',
      clients: 100,
      msgSize: 1024,
      duration: 1
    });
    const index = spawn.args.indexOf('--start-gate');
    assert.notEqual(index, -1);
    assert.equal(spawn.args[index + 1], '1');
  } finally {
    if (previous === undefined) {
      delete process.env.PERF_STREAM_CLIENT_BINARY;
    } else {
      process.env.PERF_STREAM_CLIENT_BINARY = previous;
    }
  }
});

test('STREAM server and runner resolve the same non-TCP client cap', () => {
  const environment = { PERF_STREAM_NON_TCP_CLIENTS_MAX: '8' };
  assert.equal(resolveMultiStreamClientCount(100, 'wss', environment), 8);
  assert.equal(resolveMultiStreamClientCount(100, 'tcp', environment), 100);
  assert.equal(resolveMultiStreamClientCount(100, 'wss', {
    PERF_STREAM_NON_TCP_CLIENTS_MAX: 'invalid'
  }), 100);
});

test('runner does not release STREAM client before exact server ACK', async () => {
  const server = fakeManagedProcess();
  const client = fakeManagedProcess();
  let serverInput = '';
  let clientInput = '';
  server.stdin.on('data', (chunk) => { serverInput += chunk.toString(); });
  client.stdin.on('data', (chunk) => { clientInput += chunk.toString(); });

  const barrier = coordinateRunnerStart(server, client, {
    pattern: 'STREAM',
    msgSize: 1024,
    connectReadyTimeoutMs: 1000
  }, 'stream-server');
  assert.equal(serverInput, 'START,1024\n');
  assert.equal(clientInput, '');

  server.stdout.write('SERVER_START_READY,1024\n');
  await barrier;
  assert.equal(clientInput, 'START,1024\n');
});

test('runner fails a mismatched STREAM server ACK', async () => {
  const server = fakeManagedProcess();
  const client = fakeManagedProcess();
  const barrier = coordinateRunnerStart(server, client, {
    pattern: 'STREAM',
    msgSize: 1024,
    connectReadyTimeoutMs: 1000
  }, 'stream-server');
  server.stdout.write('SERVER_START_READY,256\n');
  await assert.rejects(barrier, /token mismatch/);
});

test('STREAM server keeps one control reader across START and STOP', async () => {
  const input = new EventEmitter();
  const barrier = createStreamControlBarrier(input, 1024, 1000);
  input.emit('line', 'START,1024');
  await barrier.start;
  assert.equal(barrier.stopRequested(), false);
  input.emit('line', 'STOP');
  assert.equal(barrier.stopRequested(), true);
  barrier.close();
});
