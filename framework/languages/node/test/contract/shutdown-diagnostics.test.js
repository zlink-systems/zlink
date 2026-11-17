const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const ts = require('typescript');
const { traceShutdownStep } = require('../../packages/framework/dist/runtime/diagnostics');
const { stopRuntimeParts } = require('../../packages/framework/dist/runtime/host/runtime-shutdown');

for (const enabled of [false, true]) {
  test(`shutdown diagnostics ${enabled ? 'on' : 'off'} retain original errors and cleanup order`, async () => {
    const previous = process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
    const originalConsole = console.error;
    const events = [];
    const calls = [];
    const failure = new Error('store cleanup failure');
    const listenerFailure = Promise.reject(failure);
    process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = enabled ? '1' : '0';
    console.error = (...args) => events.push(args);
    try {
      await assert.rejects(stopRuntimeParts({
        state: {
          listenerTasks: [listenerFailure],
          dispose: async () => calls.push('state'),
          errorSink: { reportRuntimeTaskException: (_name, error) => assert.equal(error, failure) }
        },
        locationSnapshot: {},
        streamRuntime: { dispose: async () => calls.push('streams') },
        ownedStores: new Map([[{ dispose: () => { calls.push('store'); throw failure; } },
          { run: operation => operation() }]])
      }), error => error === failure);
      assert.deepEqual(calls, ['streams', 'state', 'store']);
      await assert.rejects(traceShutdownStep('session_barrier.test', () => { throw failure; }, 'play'), error => error === failure);
      if (enabled) {
        assert.deepEqual(events.filter(([, , detail]) => detail.stage === 'owner_cleanup.store').map(([, , detail]) => detail.outcome), ['started', 'failed']);
        assert.equal(events.find(([, , detail]) => detail.stage === 'owner_cleanup.store' && detail.outcome === 'failed')[2].error, failure);
        assert.equal(events.find(([, , detail]) => detail.stage === 'transport_cleanup.listener_task' && detail.outcome === 'failed')[2].error, failure);
        assert.deepEqual(events.filter(([, , detail]) => detail.stage === 'transport_cleanup.streams').map(([, , detail]) => detail.outcome), ['started', 'completed']);
        assert.equal(events.at(-1)[2].meshName, 'play');
      } else assert.equal(events.length, 0);
    } finally {
      console.error = originalConsole;
      if (previous === undefined) delete process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
      else process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = previous;
    }
  });
}

function assertDisabledPathDoesNotAllocate(source) {
  const ast = ts.createSourceFile('diagnostics.ts', source, ts.ScriptTarget.Latest, true);
  const fn = ast.statements.find(node => ts.isFunctionDeclaration(node) && node.name?.text === 'traceShutdownStep');
  const first = fn.body.statements[0];
  assert.ok(ts.isIfStatement(first), 'disabled path must return before diagnostic construction');
  assert.ok(ts.isPrefixUnaryExpression(first.expression));
  assert.equal(first.expression.operator, ts.SyntaxKind.ExclamationToken);
  assert.equal(first.expression.operand.expression.text, 'isRelocationDebugEnabled');
  assert.ok(ts.isReturnStatement(first.thenStatement));
  assert.equal(first.thenStatement.expression.expression.text, 'operation');
  const allocations = [];
  function visit(node) {
    if (ts.isObjectLiteralExpression(node) || ts.isTemplateExpression(node) || ts.isNewExpression(node)) allocations.push(node);
    ts.forEachChild(node, visit);
  }
  visit(first);
  assert.equal(allocations.length, 0);
}

test('disabled shutdown diagnostics return before constructing detail or strings', () => {
  const source = fs.readFileSync(require.resolve('../../packages/framework/src/runtime/diagnostics/index.ts'), 'utf8');
  assertDisabledPathDoesNotAllocate(source);
  const allocating = source.replace('if (!isRelocationDebugEnabled()) return operation();', 'const detail = {}; if (!isRelocationDebugEnabled()) return operation();');
  assert.notEqual(allocating, source);
  assert.throws(() => assertDisabledPathDoesNotAllocate(allocating), /disabled path must return/);
});
