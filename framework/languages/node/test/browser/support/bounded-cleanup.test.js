'use strict';

const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const test = require('node:test');
const { closeBrowser, closeBrowserServer, closeContext, closeServer, stopChild } = require('./bounded-cleanup');

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
