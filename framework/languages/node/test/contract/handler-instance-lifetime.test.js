const assert = require('node:assert/strict');
const test = require('node:test');

const {
  disposeLifecycleHandlers,
  resolveLifecycleHandler,
  runInHandlerInstanceScope,
  runWithLifecycleHandler
} = require('../../packages/framework/dist/runtime/handlers/handler-instance-scope');

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
  assert.notEqual(
    await resolveLifecycleHandler(secondActivation, Handler, resolver),
    first
  );

  await disposeLifecycleHandlers(firstActivation);
  await disposeLifecycleHandlers(firstActivation);
  await assert.rejects(
    resolveLifecycleHandler(firstActivation, Handler, resolver),
    /closing/
  );
  await disposeLifecycleHandlers(secondActivation);

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

  const disposal = disposeLifecycleHandlers(activation);
  await new Promise((resolve) => setImmediate(resolve));
  releaseCreation();

  await assert.rejects(
    resolution,
    /disposed during activation/
  );
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
  await runWithLifecycleHandler(
    activation,
    Handler,
    resolver,
    async () => {
      await disposeLifecycleHandlers(activation);
      assert.equal(disposes, 0);
    }
  );
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(disposes, 1);
  await assert.rejects(
    resolveLifecycleHandler(activation, Handler, resolver),
    /closing/
  );
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
    () => new Promise((resolve) => {
      releaseInvocation = resolve;
    })
  );
  await new Promise((resolve) => setImmediate(resolve));

  let cleanupCompleted = false;
  const disposal = disposeLifecycleHandlers(activation)
    .then(() => {
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
