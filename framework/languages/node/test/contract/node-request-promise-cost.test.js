'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const { frameworkPair } = require('../../perf/request-backpressure.cjs');

// Attribute to modules, rather than names that disappear when code is removed.
const OWNERS = [
  '/runtime/backend/mesh-completion-table.js',
  '/runtime/backend/node/node-raw-binding-port.js',
  '/runtime/messaging/request-result.js'
];
test('typed remote requests use two Framework completion Promises per reply', async (t) => {
  const pair = await frameworkPair();
  const count = 100;
  let active = false;
  let inspecting = false;
  let framework = 0;
  let binding = 0;
  const sites = new Map();
  const hook = createHook({
    init(_id, type) {
      if (!active || inspecting || type !== 'PROMISE') return;
      inspecting = true;
      try {
        const stack = new Error().stack.replaceAll('\\', '/').split('\n');
        const owner = stack.find((line) => OWNERS.some((module) => line.includes(module)));
        if (owner) {
          framework++;
          sites.set(owner, (sites.get(owner) ?? 0) + 1);
        }
        const bindingOwner = stack.find((line) => line.includes('/zlink/'));
        if (bindingOwner?.includes('/completion_owner.js')) binding++;
      } finally {
        inspecting = false;
      }
    }
  });
  try {
    for (let i = 0; i < 20; i++) {
      const request = pair.submit();
      pair.validate(await request.reply);
    }
    hook.enable();
    active = true;
    for (let i = 0; i < count; i++) {
      const request = pair.submit();
      pair.validate(await request.reply);
    }
    active = false;
    hook.disable();
    t.diagnostic(JSON.stringify({ framework, binding, count, sites: [...sites] }));
    // This positive control catches a disabled sampler or an invalid path filter.
    assert.ok(binding >= 4 * count, 'binding completion allocation control');
    assert.equal(framework, 2 * count, 'one binding observation and one public reply per request');
  } finally {
    active = false;
    hook.disable();
    await pair.close();
  }
});
