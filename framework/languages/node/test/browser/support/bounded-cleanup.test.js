'use strict';

const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const test = require('node:test');
const { stopChild } = require('./bounded-cleanup');

test('stopChild waits for a normally exiting child', async () => {
  const child = new EventEmitter();
  child.exitCode = null;
  child.signalCode = null;
  child.kill = () => {
    child.exitCode = 0;
    child.emit('exit', 0, null);
  };
  assert.deepEqual(await stopChild(child, 20), { forced: false });
});

test('stopChild force-kills a child that does not exit', async () => {
  const child = new EventEmitter();
  child.exitCode = null;
  child.signalCode = null;
  const signals = [];
  child.kill = (signal) => signals.push(signal);
  assert.deepEqual(await stopChild(child, 5), { forced: true });
  assert.deepEqual(signals, ['SIGTERM', 'SIGKILL']);
});
