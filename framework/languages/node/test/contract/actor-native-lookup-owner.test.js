const assert = require('node:assert/strict');
const test = require('node:test');
const { constants } = require('node:os');
const { ConfigError, ConfigResult } = require('@zlink-systems/zlink');
const {
  lookupNativeActorRef
} = require('../../packages/framework/dist/runtime/actors/actor-native-lookup');

test('native Actor lookup observes binding NotFound independently of errno', () => {
  const node = {
    actorLookup() {
      throw new ConfigError(ConfigResult.NotFound);
    }
  };
  assert.equal(lookupNativeActorRef(node, 'missing'), undefined);
});

test('native Actor lookup preserves binding failures whose errno resembles NotFound', () => {
  const failure = new ConfigError(ConfigResult.Busy, constants.errno.ENOENT);
  const node = {
    actorLookup() {
      throw failure;
    }
  };
  assert.throws(
    () => lookupNativeActorRef(node, 'actor'),
    (error) => error === failure
  );
});

test('native Actor lookup preserves errors without a binding result', () => {
  const failure = Object.assign(new Error('application failure'), {
    nativeErrno: constants.errno.ENOENT
  });
  const node = {
    actorLookup() {
      throw failure;
    }
  };
  assert.throws(
    () => lookupNativeActorRef(node, 'actor'),
    (error) => error === failure
  );
});

test('native Actor lookup preserves found and absent values', () => {
  const actor = { nodeRid: 'node', actorId: 'actor', generation: 1n };
  assert.deepEqual(lookupNativeActorRef({ actorLookup: () => ({ actor }) }, 'actor'), actor);
  assert.equal(lookupNativeActorRef({ actorLookup: () => undefined }, 'actor'), undefined);
});
