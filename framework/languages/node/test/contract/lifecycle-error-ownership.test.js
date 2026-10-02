const assert = require('node:assert/strict');
const test = require('node:test');
const { spawnSync } = require('node:child_process');
const { ZLinkSpotActivationLifecycle } = require('../../packages/framework/dist/runtime/spots/spot-activation');
const { DefaultZLinkActorManager } = require('../../packages/framework/dist/runtime/actors');
const { ZLinkEntrySpotActivation } = require('../../packages/framework/dist/runtime/spots/spot-entry-activation');
const { ZLinkRuntimeTaskErrorSink, ZLinkRuntimeTaskRunner } = require('../../packages/framework/dist/runtime/execution');
const { resolveLifecycleHandler } = require('../../packages/framework/dist/runtime/handlers/handler-instance-scope');

const runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeTaskErrorSink(), new AbortController().signal);

test('partial Spot creation attempts location cleanup when native cleanup throws synchronously', async () => {
  const original = new Error('provider creation failed');
  const nativeFailure = new Error('native cleanup failed');
  const locationFailure = new Error('location cleanup failed');
  const cleaned = [];
  class Spot {}
  const lifecycle = new ZLinkSpotActivationLifecycle({
    detachedTaskRunner: runner,
    providerResolver: { create() { throw original; } },
    createNativeSpot: () => ({
      lifecycleGeneration: 1n,
      dispose() { cleaned.push('native'); throw nativeFailure; }
    }),
    locationClaim: {
      enabled: true,
      claimUserSpot: async () => ({ claimed: true, meshName: 'r3-mesh' }),
      release() { cleaned.push('location'); throw locationFailure; }
    }
  });
  await assert.rejects(lifecycle.create('r3-mesh', Spot, 'r3-partial', {}), error => {
    assert.ok(error instanceof AggregateError);
    assert.equal(error.errors[0], original);
    assert.deepEqual(error.errors[1].errors, [nativeFailure, locationFailure]);
    return true;
  });
  assert.deepEqual(cleaned.sort(), ['location', 'native']);
});

test('lifecycle owners reject a missing detached task runner during construction', () => {
  assert.throws(() => new DefaultZLinkActorManager({}), /detached task runner/i);
  assert.throws(() => new ZLinkSpotActivationLifecycle({}), /detached task runner/i);
  assert.throws(() => new ZLinkEntrySpotActivation({}), /detached task runner/i);
});

test('self disposal rejects a missing runner before starting cleanup without an unhandled rejection', () => {
  const scopePath = require.resolve('../../packages/framework/dist/runtime/handlers/handler-instance-scope');
  const executionPath = require.resolve('../../packages/framework/dist/runtime/execution');
  const child = spawnSync(process.execPath, ['-e', `
    const assert = require('node:assert/strict');
    const { runWithLifecycleHandler, disposeLifecycleHandlers } = require(${JSON.stringify(scopePath)});
    const { ZLinkRuntimeTaskErrorSink, ZLinkRuntimeTaskRunner } = require(${JSON.stringify(executionPath)});
    const sink = new ZLinkRuntimeTaskErrorSink();
    const errors = [];
    sink.onRuntimeTaskException(({ error }) => errors.push(error));
    const runner = new ZLinkRuntimeTaskRunner(sink, new AbortController().signal);
    const failure = new Error('release failure');
    const owner = {};
    class Handler { dispose() { throw failure; } }
    (async () => {
      await runWithLifecycleHandler(owner, Handler, undefined, async () => {
        await assert.rejects(disposeLifecycleHandlers(owner), /detached task runner/i);
      });
      await assert.rejects(disposeLifecycleHandlers(owner, runner), error => error === failure);
      assert.deepEqual(errors, []);
    })().catch(error => { console.error(error); process.exitCode = 1; });
  `], { encoding: 'utf8', timeout: 5000 });
  assert.equal(child.status, 0, child.stdout + child.stderr);
});

for (const operation of ['relocation', 'relocation-sync-native', 'instance']) {
  test(`${operation} startup preserves the original failure and every cleanup failure`, async () => {
    const original = new Error('startup failed');
    const timerFailure = new Error('timer cleanup failed');
    const handlerFailure = new Error('handler cleanup failed');
    const nativeFailure = new Error('native cleanup failed');
    const cleaned = [];
    class Handler { dispose() { cleaned.push('handler'); throw handlerFailure; } }
    class TimerHandler { handle() {} }
    class Spot {
      async configure() {
        await resolveLifecycleHandler(this, Handler);
        await this.context.addTimer('startup-timer', 1000, TimerHandler);
        throw original;
      }
    }
    const lifecycle = new ZLinkSpotActivationLifecycle({
      detachedTaskRunner: runner,
      timerClock: {
        now: () => 1,
        utcNow: () => 1,
        setTimeout: () => ({}),
        clearTimeout() { cleaned.push('timer'); throw timerFailure; }
      },
      createNativeSpot: () => ({
        dispose() {
          cleaned.push('native');
          if (operation === 'relocation-sync-native') throw nativeFailure;
          return Promise.reject(nativeFailure);
        }
      }),
      registerActivation() { assert.fail('failed startup must not register an activation'); }
    });
    const pending = operation !== 'instance'
      ? lifecycle.materializeRelocation('r3-mesh', 'user_spot', 'r3-spot', Spot, 'r3-spot', 1n, 1n)
      : lifecycle.materializeInstance('r3-mesh', 'r3-spot', Spot, 'r3-spot', 1n);
    await assert.rejects(pending, error => {
      assert.ok(error instanceof AggregateError);
      assert.deepEqual(error.errors, operation !== 'instance'
        ? [original, timerFailure, handlerFailure, nativeFailure]
        : [original, timerFailure, handlerFailure]);
      return true;
    });
    assert.deepEqual(cleaned.sort(), operation !== 'instance' ? ['handler', 'native', 'timer'] : ['handler', 'timer']);
  });
}
