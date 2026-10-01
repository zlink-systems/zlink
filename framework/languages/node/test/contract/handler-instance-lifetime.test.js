const assert = require('node:assert/strict');
const test = require('node:test');

const {
  disposeLifecycleHandlers,
  resolveLifecycleHandler,
  runInHandlerInstanceScope,
  runWithLifecycleHandler
} = require('../../packages/framework/dist/runtime/handlers/handler-instance-scope');

const {
  ZLinkRuntimeTaskErrorSink,
  ZLinkRuntimeTaskRunner
} = require('../../packages/framework/dist/runtime/execution');

const lifecycleRunner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeTaskErrorSink(), new AbortController().signal);

test('self teardown reports a release failure through the existing detached task runner once', async () => {
  const failure = new Error('self teardown release failed');
  const errors = [];
  const sink = new ZLinkRuntimeTaskErrorSink();
  sink.onRuntimeTaskException((reported) => errors.push(reported.error));
  const runner = new ZLinkRuntimeTaskRunner(sink, new AbortController().signal);
  const tasks = [];
  const detached = {
    runDetached: (name, callback) => tasks.push(runner.run(name, callback))
  };
  let releaseStarted;
  const released = new Promise((resolve) => {
    releaseStarted = resolve;
  });
  let releases = 0;
  class Handler {
    dispose() {
      releases += 1;
      releaseStarted();
      throw failure;
    }
  }
  const activation = {};
  await runWithLifecycleHandler(activation, Handler, undefined, async () => {
    await disposeLifecycleHandlers(activation, detached);
    await disposeLifecycleHandlers(activation, detached);
    assert.equal(releases, 0);
  });
  await released;
  await Promise.all(tasks);
  assert.deepEqual(errors, [failure]);
  assert.equal(releases, 1);
  await disposeLifecycleHandlers(activation, detached);
  assert.equal(releases, 1);
});

test('external teardown retains its release failure without failing the handler or detaching a task', async () => {
  const failure = new Error('external teardown release failed');
  let finishHandler;
  const handlerCanFinish = new Promise((resolve) => {
    finishHandler = resolve;
  });
  let handlerStarted;
  const started = new Promise((resolve) => {
    handlerStarted = resolve;
  });
  const tasks = [];
  const detached = { runDetached: (...args) => tasks.push(args) };
  class Handler {
    dispose() {
      throw failure;
    }
  }
  const activation = {};
  const invocation = runWithLifecycleHandler(activation, Handler, undefined, async () => {
    handlerStarted();
    await handlerCanFinish;
    return 'handled';
  });
  await started;
  const disposal = disposeLifecycleHandlers(activation, detached);
  const rejected = assert.rejects(disposal, (error) => error === failure);
  finishHandler();
  assert.equal(await invocation, 'handled');
  await rejected;
  assert.deepEqual(tasks, []);
  await disposeLifecycleHandlers(activation, detached);
});

test('self teardown preserves the handler failure while reporting the release failure separately', async () => {
  const handlerFailure = new Error('handler failed');
  const releaseFailure = new Error('release failed');
  const errors = [];
  const tasks = [];
  const sink = new ZLinkRuntimeTaskErrorSink();
  sink.onRuntimeTaskException(({ error }) => errors.push(error));
  const runner = new ZLinkRuntimeTaskRunner(sink, new AbortController().signal);
  const detached = { runDetached: (name, callback) => tasks.push(runner.run(name, callback)) };
  class Handler {
    dispose() {
      throw releaseFailure;
    }
  }
  const activation = {};
  await assert.rejects(
    runWithLifecycleHandler(activation, Handler, undefined, async () => {
      await disposeLifecycleHandlers(activation, detached);
      throw handlerFailure;
    }),
    (error) => error === handlerFailure
  );
  await Promise.all(tasks);
  assert.deepEqual(errors, [releaseFailure]);
});

test('scope cleanup releases earlier instances after a later release fails', async () => {
  const released = [];
  const failure = new Error('handler release failed');
  class First {
    dispose() {
      released.push('first');
    }
  }
  class Last {
    dispose() {
      released.push('last');
      throw failure;
    }
  }
  await assert.rejects(
    runInHandlerInstanceScope(undefined, undefined, async (scope) => {
      await scope.resolve(First);
      await scope.resolve(Last);
    }),
    failure
  );
  assert.deepEqual(released, ['last', 'first']);
});

test('handler activation starts in the scope caller before returning its promise', async () => {
  let created = false;
  class Handler {}
  await runInHandlerInstanceScope(
    {
      create(type) {
        created = true;
        return new type();
      }
    },
    undefined,
    async (scope) => {
      const instance = scope.resolve(Handler);
      const started = created;
      assert.ok((await instance) instanceof Handler);
      assert.equal(started, true);
    }
  );
});

test('default constructor starts before returning the resolution promise', async () => {
  let created = false;
  class Handler {
    constructor() {
      created = true;
    }
  }
  await runInHandlerInstanceScope(undefined, undefined, async (scope) => {
    const pending = scope.resolve(Handler);
    const started = created;
    await pending;
    assert.equal(started, true);
  });
});

test('synchronous provider reentry observes the admitted activation promise', async () => {
  let currentScope;
  let nested;
  let creates = 0;
  class Handler {}
  const resolver = {
    create(type) {
      creates += 1;
      nested = currentScope.resolve(type);
      return new type();
    }
  };
  await runInHandlerInstanceScope(resolver, undefined, async (scope) => {
    currentScope = scope;
    const pending = scope.resolve(Handler);
    assert.equal(await pending, await nested);
    assert.equal(creates, 1);
  });
});

for (const entry of [resolveLifecycleHandler, runWithLifecycleHandler]) {
  test(`${entry.name} starts provider creation in its caller`, async () => {
    const owner = {};
    let created = false;
    class Handler {}
    const pending = entry(
      owner,
      Handler,
      {
        create(type) {
          created = true;
          return new type();
        }
      },
      async (handler) => handler
    );
    const started = created;
    await pending;
    assert.equal(started, true);
    await disposeLifecycleHandlers(owner, lifecycleRunner);
  });
}

test('channel handler scope creates one instance per dispatch and disposes it once', async () => {
  let creates = 0;
  let gets = 0;
  let disposes = 0;

  class Handler {
    constructor() {
      this.id = ++creates;
    }

    dispose() {
      disposes += 1;
    }
  }

  const singleton = new Handler();
  creates = 0;
  const resolver = {
    get() {
      gets += 1;
      return singleton;
    },
    create(type) {
      return new type();
    }
  };

  const firstContext = { channelName: 'api', packetName: 'Ping', metadata: new Map() };
  await runInHandlerInstanceScope(resolver, firstContext, async (scope) => {
    const first = await scope.resolve(Handler);
    const second = await scope.resolve(Handler);
    assert.equal(first, second);
    assert.notEqual(first, singleton);
  });

  const secondContext = { channelName: 'api', packetName: 'Ping', metadata: new Map() };
  await runInHandlerInstanceScope(resolver, secondContext, async (scope) => {
    assert.equal((await scope.resolve(Handler)).id, 2);
  });

  assert.equal(creates, 2);
  assert.equal(gets, 0);
  assert.equal(disposes, 2);
});

test('lifecycle handler scope is retained by its activation owner and disposed exactly once', async () => {
  let creates = 0;
  let disposes = 0;

  class Handler {
    constructor() {
      this.id = ++creates;
    }

    async onModuleDestroy() {
      disposes += 1;
    }
  }

  const resolver = { create: (type) => new type() };
  const firstActivation = {};
  const secondActivation = {};

  const first = await resolveLifecycleHandler(firstActivation, Handler, resolver);
  assert.equal(await resolveLifecycleHandler(firstActivation, Handler, resolver), first);
  assert.notEqual(await resolveLifecycleHandler(secondActivation, Handler, resolver), first);

  await disposeLifecycleHandlers(firstActivation, lifecycleRunner);
  await disposeLifecycleHandlers(firstActivation, lifecycleRunner);
  await assert.rejects(resolveLifecycleHandler(firstActivation, Handler, resolver), /closing/);
  await disposeLifecycleHandlers(secondActivation, lifecycleRunner);

  assert.equal(creates, 2);
  assert.equal(disposes, 2);
});

test('lifecycle disposal waits for pending creation and releases the late instance', async () => {
  let releaseCreation;
  let disposes = 0;

  class Handler {
    dispose() {
      disposes += 1;
    }
  }

  const resolver = {
    create() {
      return new Promise((resolve) => {
        releaseCreation = () => resolve(new Handler());
      });
    }
  };
  const activation = {};
  const resolution = resolveLifecycleHandler(activation, Handler, resolver);
  await new Promise((resolve) => setImmediate(resolve));

  const disposal = disposeLifecycleHandlers(activation, lifecycleRunner);
  await new Promise((resolve) => setImmediate(resolve));
  releaseCreation();

  await assert.rejects(resolution, /disposed during activation/);
  await disposal;
  assert.equal(disposes, 1);
});

test('handler can initiate its own lifecycle teardown without waiting for itself', async () => {
  let disposes = 0;

  class Handler {
    dispose() {
      disposes += 1;
    }
  }

  const activation = {};
  const resolver = { create: (type) => new type() };
  await runWithLifecycleHandler(activation, Handler, resolver, async () => {
    await disposeLifecycleHandlers(activation, lifecycleRunner);
    assert.equal(disposes, 0);
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(disposes, 1);
  await assert.rejects(resolveLifecycleHandler(activation, Handler, resolver), /closing/);
});

test('lifecycle disposal waits for the active handler invocation', async () => {
  let releaseInvocation;
  let disposes = 0;

  class Handler {
    dispose() {
      disposes += 1;
    }
  }

  const activation = {};
  const resolver = { create: (type) => new type() };
  const invocation = runWithLifecycleHandler(
    activation,
    Handler,
    resolver,
    () =>
      new Promise((resolve) => {
        releaseInvocation = resolve;
      })
  );
  await new Promise((resolve) => setImmediate(resolve));

  let cleanupCompleted = false;
  const disposal = disposeLifecycleHandlers(activation, lifecycleRunner).then(() => {
    cleanupCompleted = true;
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(cleanupCompleted, false);
  assert.equal(disposes, 0);

  releaseInvocation();
  await invocation;
  await disposal;
  assert.equal(disposes, 1);
});
