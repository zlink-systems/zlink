// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { PassThrough } = require('node:stream');
const { attachProcessCapture, buildClientSpawn, coordinateRunnerStart } = require('../perf/multi/perf_multi_orchestrator');
const { createStreamControlBarrier } = require('../perf/multi/perf_multi_stream_server');
const { resolveMultiStreamClientCount } = require('../perf/multi/perf_multi_common');
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
test('shared STREAM client is spawned behind the START gate', () => {
    const previous = process.env.PERF_STREAM_CLIENT_BINARY;
    process.env.PERF_STREAM_CLIENT_BINARY = '/tmp/perf_stream_client';
    try {
        const spawn = buildClientSpawn(null, ['--endpoint', 'tcp://127.0.0.1:5555'], {
            pattern: 'MULTI_STREAM',
            transport: 'tcp',
            clients: 100,
            msgSize: 1024,
            duration: 1
        });
        const index = spawn.args.indexOf('--start-gate');
        assert.notEqual(index, -1);
        assert.equal(spawn.args[index + 1], '1');
    }
    finally {
        if (previous === undefined) {
            delete process.env.PERF_STREAM_CLIENT_BINARY;
        }
        else {
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
        pattern: 'MULTI_STREAM',
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
        pattern: 'MULTI_STREAM',
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
