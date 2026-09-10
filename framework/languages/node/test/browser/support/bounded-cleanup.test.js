'use strict';

const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const test = require('node:test');
const { closeBrowser, closeServer, stopChild } = require('./bounded-cleanup');

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

test('closeServer is bounded when its close callback hangs', async () => {
  const server = { listening: true, close: () => {}, closeAllConnections() {} };
  assert.deepEqual(await closeServer(server, 5), { timedOut: true, forced: true });
});

test('closeBrowser is bounded when browser.close hangs', async () => {
  const browser = { close: () => new Promise(() => {}) };
  assert.deepEqual(await closeBrowser(browser, 5), { timedOut: true, forced: false });
});
