'use strict';

const assert = require('node:assert/strict');
const net = require('node:net');
const test = require('node:test');

// A port picked by closing a probe listener is free only until another process
// takes it, so fixtures bind port 0 and read the address the listener holds.
function spyOnNetListeners() {
  const originalListen = net.Server.prototype.listen;
  const calls = [];
  net.Server.prototype.listen = function observedListen(...args) {
    calls.push(args);
    return Reflect.apply(originalListen, this, args);
  };
  return {
    calls,
    restore() {
      net.Server.prototype.listen = originalListen;
    }
  };
}

test('request backpressure pairs bind port 0 and connect to the listener address', async () => {
  const spy = spyOnNetListeners();
  try {
    const { frameworkPair, bindingPair } = require('../../perf/request-backpressure.cjs');
    const framework = await frameworkPair('normal');
    try {
      framework.validate(await framework.submit().reply);
    } finally {
      await framework.close();
    }
    const binding = await bindingPair();
    await binding.close();
  } finally {
    spy.restore();
  }
  assert.deepEqual(spy.calls, [], 'the fixture must not reserve a port through a probe listener');
});
