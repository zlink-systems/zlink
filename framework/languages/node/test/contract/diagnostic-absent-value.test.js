const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const diagnosticText = require('../../packages/framework/dist/runtime/diagnostics/diagnostic-text');

test('absent diagnostic text is emitted as <none> on the enabled relocation debug path', () => {
  const previousDebug = process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
  const originalError = console.error;
  const calls = [];
  process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = '1';
  console.error = (...args) => calls.push(args);
  try {
    framework.emitActorOwnerLeaseObservation({
      actorId: diagnosticText.diagnosticTextOrAbsent(undefined),
      authorityGeneration: 7n,
      remainingLeaseMs: 0,
      decision: 'owner_unavailable'
    });
  } finally {
    console.error = originalError;
    if (previousDebug === undefined) {
      delete process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
    } else {
      process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = previousDebug;
    }
  }

  assert.equal(calls.length, 1);
  assert.equal(calls[0][2].actorId, '<none>');
  assert.equal(diagnosticText.diagnosticTextOrAbsent(null), '<none>');
});

test('actor lifecycle diagnostics use <none> when the actor id is absent', () => {
  assert.throws(
    () => framework.actorJoinIdentity({ context: {} }),
    (error) => error.message.includes("Actor '<none>'")
  );
});
