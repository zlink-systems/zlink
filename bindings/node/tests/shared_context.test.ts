'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { Worker } = require('node:worker_threads');
const { spawnSync } = require('node:child_process');
const zlink = require('@zlink-systems/zlink');

test('shared context returns one wrapper without termination methods', () => {
  const first = zlink.sharedContext();
  assert.strictEqual(zlink.sharedContext(), first);
  assert.equal('close' in first, false);
  assert.equal('shutdown' in first, false);
  assert.equal(first.options.threadNamePrefix, '');
  first.options.threadNamePrefix = 'shared';
  assert.equal(first.options.threadNamePrefix, 'shared');
  first.options.threadNamePrefix = '';
});

test('two Workers exchange PAIR messages through the shared context', async () => {
  const endpoint = `inproc://node-shared-${process.pid}-${Date.now()}`;
  const source = `
    const { parentPort, workerData } = require('node:worker_threads');
    const zlink = require('@zlink-systems/zlink');
    const socket = zlink.createPairSocket(zlink.sharedContext());
    if (workerData.bind) socket.bind(workerData.endpoint);
    else socket.connect(workerData.endpoint);
    const received = new zlink.Received();
    socket.send().message(workerData.bind ? 'left' : 'right').submit_sync();
    socket.recv(received);
    parentPort.postMessage(received.parts[0].data().toString());
    received.close();
    socket.close();
  `;
  const run = (bind: boolean) => new Promise<string>((resolve, reject) => {
    const worker = new Worker(source, { eval: true, workerData: { bind, endpoint } });
    let message: string | undefined;
    worker.on('message', (value: string) => { message = value; });
    worker.on('error', reject);
    worker.on('exit', (code: number) => code === 0 && message
      ? resolve(message) : reject(new Error(`worker exited ${code}: ${message}`)));
  });
  const [left, right] = await Promise.all([run(true), run(false)]);
  assert.deepEqual([left, right], ['right', 'left']);
});

test('process exits after Worker cleanup and main socket close', () => {
  const script = `
    const { Worker } = require('node:worker_threads');
    const zlink = require('@zlink-systems/zlink');
    const endpoint = 'inproc://node-shared-exit-' + process.pid;
    const main = [0, 1].map(index => {
      const socket = zlink.createPairSocket(zlink.sharedContext());
      socket.bind(endpoint + '-' + index);
      return socket;
    });
    let remaining = main.length;
    for (let index = 0; index < main.length; index++) {
      const worker = new Worker(\`
      const { parentPort, workerData } = require('node:worker_threads');
      const zlink = require('@zlink-systems/zlink');
      const socket = zlink.createPairSocket(zlink.sharedContext());
      socket.connect(workerData);
      socket.send().message('done').submit_sync();
      parentPort.postMessage('sent');
      // The addon cleanup owns this socket when the Worker exits.
      \`, { eval: true, workerData: endpoint + '-' + index });
      worker.on('error', error => { throw error; });
      worker.on('exit', code => {
        if (code !== 0) process.exitCode = 1;
        if (--remaining === 0) main.forEach(socket => socket.close());
      });
    }
    for (const socket of main) {
      const received = new zlink.Received();
      socket.recv(received);
      if (received.parts[0].data().toString() !== 'done') process.exitCode = 1;
      received.close();
    }
  `;
  const child = spawnSync(process.execPath, ['-e', script], {
    cwd: process.cwd(), encoding: 'utf8', timeout: 8_000,
  });
  assert.equal(child.error, undefined, String(child.error));
  assert.equal(child.status, 0, child.stderr);
});
