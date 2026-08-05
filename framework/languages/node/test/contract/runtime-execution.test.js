const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

test('runtime task runner observes detached task exceptions without unhandled rejection', async () => {
  const abortController = new AbortController();
  const sink = new framework.ZLinkRuntimeTaskErrorSink();
  const runner = new framework.ZLinkRuntimeTaskRunner(sink, abortController.signal);
  const observed = [];
  const unhandled = [];
  const onUnhandled = (reason) => unhandled.push(reason);
  process.on('unhandledRejection', onUnhandled);
  try {
    sink.onRuntimeTaskException((failure) => observed.push(failure));

    runner.runDetached('detached-test', async () => {
      throw new Error('detached failed');
    });
    await new Promise((resolve) => setImmediate(resolve));
    await new Promise((resolve) => setImmediate(resolve));

    assert.equal(unhandled.length, 0);
    assert.equal(observed.length, 1);
    assert.equal(observed[0].taskName, 'detached-test');
    assert.equal(observed[0].error.message, 'detached failed');
  } finally {
    process.off('unhandledRejection', onUnhandled);
    abortController.abort();
  }
});

test('framework execution state aborts listener tasks before disposing backend context', async () => {
  const events = [];
  const state = new framework.ZLinkFrameworkExecutionState({
    async dispose() {
      events.push('context:dispose');
    }
  });
  state.listenerTasks.push(state.taskRunner.run('listener', async (signal) => {
    while (!signal.aborted) {
      await new Promise((resolve) => setImmediate(resolve));
    }
    events.push('listener:aborted');
  }));

  await state.dispose();

  assert.deepEqual(events, ['listener:aborted', 'context:dispose']);
});
