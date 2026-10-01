'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const lane = require('../../packages/framework/dist/runtime/execution/state-lane');
const diagnostics = require('../../packages/framework/dist/runtime/diagnostics');

function restoreEnvironment(name, value) {
  if (value === undefined) delete process.env[name];
  else process.env[name] = value;
}

test('structural guard decision observes current environment without caching', () => {
  const guard = process.env.ZLINK_NODE_STRUCTURAL_GUARD;
  const environment = process.env.NODE_ENV;
  try {
    delete process.env.ZLINK_NODE_STRUCTURAL_GUARD;
    delete process.env.NODE_ENV;
    assert.equal(lane.isStructuralGuardEnabled(), false);
    process.env.ZLINK_NODE_STRUCTURAL_GUARD = '1';
    assert.equal(lane.isStructuralGuardEnabled(), true);
    process.env.ZLINK_NODE_STRUCTURAL_GUARD = '0';
    assert.equal(lane.isStructuralGuardEnabled(), false);
    process.env.NODE_ENV = 'test';
    assert.equal(lane.isStructuralGuardEnabled(), true);
    process.env.NODE_ENV = 'production';
    assert.equal(lane.isStructuralGuardEnabled(), false);
  } finally {
    restoreEnvironment('ZLINK_NODE_STRUCTURAL_GUARD', guard);
    restoreEnvironment('NODE_ENV', environment);
  }
});

test('relocation debug emitter remains gated when called directly and observes live environment', () => {
  const prior = process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
  const originalError = console.error;
  const records = [];
  console.error = (...args) => records.push(args);
  try {
    delete process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
    assert.equal(diagnostics.isRelocationDebugEnabled(), false);
    diagnostics.relocationDebug('disabled', { allocation: 'caller-created' });
    assert.equal(records.length, 0);
    process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = '1';
    assert.equal(diagnostics.isRelocationDebugEnabled(), true);
    diagnostics.relocationDebug('enabled', { actorId: 'actor-1' });
    assert.equal(records.length, 1);
    process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = '0';
    diagnostics.relocationDebug('disabled-again', {});
    assert.equal(records.length, 1);
  } finally {
    console.error = originalError;
    restoreEnvironment('ZLINK_DEBUG_FRAMEWORK_RELOCATION', prior);
  }
});
