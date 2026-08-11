'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

test('public root exports raw capabilities without service projections', () => {
  for (const name of [
    'createContext',
    'createPairSocket',
    'createRouterSocket',
    'createStreamSocket',
    'createPoller',
    'createTimer',
    'Received',
    'Message',
    'StreamPacketBodyMaterialization'
  ]) {
    assert.notEqual(zlink[name], undefined, name);
  }
});

test('ESM wrapper preserves runtime contract values exported from the CJS barrel', async () => {
  const esm = await import('../dist/index.mjs');
  for (const name of ['Message', 'Received', 'TopicMessage', 'SubscriptionEvent']) {
    assert.equal(typeof esm[name], 'function', name);
  }
});

test('package exports block internal addon and runtime modules', () => {
  assert.throws(
    () => require('@zlink-systems/zlink/dist/zlink/runtime/native/native'),
    { code: 'ERR_PACKAGE_PATH_NOT_EXPORTED' }
  );
});
