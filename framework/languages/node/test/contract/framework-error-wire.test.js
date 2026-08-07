const assert = require('node:assert/strict');
const { test } = require('node:test');

const framework = require('../../packages/framework/dist/internal');

test('every internal Framework error produces a canonical stateful wire reply', () => {
  for (const kind of Object.values(framework.ZLinkFrameworkInternalErrorKind)) {
    const error = framework.createInternalFrameworkException(kind, `injected ${kind}`);
    const reply = framework.internalFrameworkWireReply(error);
    assert.equal(
      framework.isCanonicalWireReplyTerminal(reply.terminalResult, reply.failureCode),
      true,
      `${kind} produced ${reply.terminalResult}/${reply.failureCode}`
    );
  }
});

test('stateful wire replies preserve supported detail and mask unsupported detail', () => {
  const missing = framework.internalFrameworkWireReply(
    framework.createInternalFrameworkException(
      framework.ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
      'missing actor route'
    )
  );
  assert.deepEqual(missing, { terminalResult: 102, failureCode: 1 });

  const shutdown = framework.internalFrameworkWireReply(
    framework.createInternalFrameworkException(
      framework.ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
      'runtime stopped'
    )
  );
  assert.deepEqual(shutdown, { terminalResult: 105, failureCode: 17 });
});
