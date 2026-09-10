'use strict';

const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const http = require('node:http');
const test = require('node:test');
const { closeBrowser, closeBrowserServer, closeContext, closeServer, stopChild, stopChildGracefully } = require('../../support/bounded-cleanup');

test('stopChildGracefully does not signal a normally exiting child', async () => {
  const child = new EventEmitter(); child.exitCode = null; child.signalCode = null;
  const signals = []; child.kill = (signal) => signals.push(signal);
  const result = stopChildGracefully(child, async () => { child.exitCode = 0; child.emit('exit', 0); }, 20, 5);
  assert.deepEqual(await result, { timedOut: false, forced: false });
  assert.deepEqual(signals, []);
});

test('stopChildGracefully falls back to TERM/KILL after graceful timeout', async () => {
  const child = new EventEmitter(); child.exitCode = null; child.signalCode = null;
  const signals = []; child.kill = (signal) => { signals.push(signal); if (signal === 'SIGKILL') { child.exitCode = 137; child.emit('exit', 137); } };
  assert.deepEqual(await stopChildGracefully(child, async () => {}, 5, 5), { timedOut: true, forced: true });
  assert.deepEqual(signals, ['SIGTERM', 'SIGKILL']);
});

test('stopChild waits for a normally exiting child', async () => {
  const child = new EventEmitter();
  child.exitCode = null;
  child.signalCode = null;
  child.kill = () => {
    child.exitCode = 0;
    child.emit('exit', 0, null);
  };
  assert.deepEqual(await stopChild(child, 20), { timedOut: false, forced: false });
});

test('stopChild force-kills a child that does not exit', async () => {
  const child = new EventEmitter();
  child.exitCode = null;
  child.signalCode = null;
  const signals = [];
  child.kill = (signal) => signals.push(signal);
  assert.deepEqual(await stopChild(child, 5), { timedOut: true, forced: true });
  assert.deepEqual(signals, ['SIGTERM', 'SIGKILL']);
});

test('stopChild destroys streams from an already exited child', async () => {
  const child = { exitCode: 0, signalCode: null, stdout: { destroyCalled: false, destroy() { this.destroyCalled = true; } }, stderr: { destroyCalled: false, destroy() { this.destroyCalled = true; } } };
  await stopChild(child);
  assert.equal(child.stdout.destroyCalled, true);
  assert.equal(child.stderr.destroyCalled, true);
});

test('closeServer is bounded when its close callback hangs', async () => {
  const server = { listening: true, close: () => {}, closeAllConnections() {}, unrefCalled: false, unref() { this.unrefCalled = true; } };
  assert.deepEqual(await closeServer(server, 5), { timedOut: true, forced: true });
  assert.equal(server.unrefCalled, true);
});

test('closeServer receives the underlying server from a wrapper contract', async () => {
  const server = http.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const wrapper = { server, url: `http://127.0.0.1:${server.address().port}` };
  assert.equal(wrapper.server.listening, true);
  assert.deepEqual(await closeServer(wrapper.server, 20), { timedOut: false, forced: false });
  assert.equal(server.listening, false);
});

test('closeBrowser is bounded when browser.close hangs', async () => {
  const browser = { close: () => new Promise(() => {}) };
  assert.deepEqual(await closeBrowser(browser, 5), { timedOut: true, forced: false });
});

test('closeContext is bounded when context.close hangs', async () => {
  assert.deepEqual(await closeContext({ close: () => new Promise(() => {}) }, 5), { timedOut: true, forced: false });
});

test('closeBrowserServer kills when BrowserServer.close hangs', async () => {
  let killed = false;
  let killResolved = false;
  const server = { close: () => new Promise(() => {}), kill: () => new Promise((resolve) => {
    killed = true;
    setTimeout(() => { killResolved = true; resolve(); }, 2);
  }) };
  assert.deepEqual(await closeBrowserServer(server, 10), { timedOut: true, forced: true, killTimedOut: false });
  assert.equal(killed, true);
  assert.equal(killResolved, true);
});
