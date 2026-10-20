const assert = require('node:assert/strict');
const { createHook } = require('node:async_hooks');
const test = require('node:test');
const {
  ZLinkSerialExecutionQueue
} = require('../../packages/framework/dist/runtime/execution/serial-execution-queue');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

function queue() {
  return new ZLinkSerialExecutionQueue(
    async (record) => {
      try {
        record.resolve(await record.operation());
      } catch (error) {
        record.reject(error);
      } finally {
        record.release();
      }
    },
    { ownerTimeBudget: 0 }
  );
}

test('serial selection transfers the admitted record without allocating a selection copy', async () => {
  const scheduler = queue();
  const pending = scheduler.submit(() => 42);
  assert.strictEqual(scheduler.selectNext(), scheduler.application.records[0]);
  assert.equal(await pending, 42);
  await scheduler.close();
});

test('serial records use the Promise terminal functions and preserve first settlement', async () => {
  const scheduler = queue();
  const pending = scheduler.submit(() => 42);
  const record = scheduler.selectNext();
  for (const terminal of [record.resolve, record.reject]) {
    assert.match(Function.prototype.toString.call(terminal), /\[native code\]/);
  }
  record.resolve(17);
  record.reject(new Error('late rejection'));
  assert.equal(await pending, 17);
  await scheduler.close();
  const failure = new Error('operation failed');
  const rejected = queue();
  await assert.rejects(
    rejected.submit(() => {
      throw failure;
    }),
    (error) => error === failure
  );
  await rejected.close();
});

test('an omitted serial options bag reaches admission without a replacement object', async () => {
  const scheduler = queue();
  const admit = scheduler.admit;
  scheduler.admit = function (operation, options, ...rest) {
    assert.equal(options, undefined);
    return admit.call(this, operation, options, ...rest);
  };
  assert.equal(await scheduler.submit(() => 42), 42);
  await scheduler.close();
});

test('Spot terminal settlement also supplies the scheduler completion observation', async () => {
  const executor = new ZLinkSpotSerialTurnExecutor(true, undefined, { ownerTimeBudget: 0 });
  await executor.execute(async () => 1);
  let terminalObservations = 0;
  const hook = createHook({
    init(_id, type) {
      if (type !== 'PROMISE') return;
      const frames = new Error().stack.split('\n');
      if (
        frames.some((frame) => frame.includes('at Promise.then')) &&
        frames.some((frame) => frame.includes('ZLinkSpotSerialTurnExecutor.runTurn'))
      ) {
        terminalObservations++;
      }
    }
  });
  hook.enable();
  try {
    assert.equal(await executor.execute(async () => 42), 42);
    await executor.whenIdle();
  } finally {
    hook.disable();
  }
  // Two owner observations and Promise.race's two input observations.
  assert.equal(terminalObservations, 4);
});

function countDirectPromises(method, invoke) {
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (type !== 'PROMISE') return;
      const first = new Error().stack.split('\n').find((frame) => frame.includes('/dist/runtime/'));
      if (first?.includes(method)) promises++;
    }
  });
  hook.enable();
  let pending;
  try {
    pending = invoke();
  } finally {
    hook.disable();
  }
  return { pending, promises };
}

test('Actor handler invoker forwards the lifecycle result without another Promise', async () => {
  const {
    ZLinkSpotActorDispatcher
  } = require('../../packages/framework/dist/runtime/actors/spot-actor-dispatch');
  const actor = {};
  class Handler {
    async handle() {
      return 42;
    }
  }
  const dispatcher = new ZLinkSpotActorDispatcher({});
  const invoke = () =>
    dispatcher.invokeHandler(actor, { handlerType: Handler }, (handler, method) =>
      method.call(handler)
    );
  await invoke();
  const { pending, promises } = countDirectPromises(
    'ZLinkSpotActorDispatcher.invokeHandler',
    invoke
  );
  assert.equal(await pending, 42);
  assert.equal(promises, 0);
});

test('Actor execution context forwards its owned asynchronous lifetime without another Promise', async () => {
  const {
    runZLinkActorExecution,
    currentZLinkActorExecution
  } = require('../../packages/framework/dist/runtime/actors/actor-execution-context');
  const { pending, promises } = countDirectPromises('runZLinkActorExecution', () =>
    runZLinkActorExecution('actor', 'spot', async () => {
      assert.deepEqual(currentZLinkActorExecution(), { actorId: 'actor', spotId: 'spot' });
      return 42;
    })
  );
  assert.equal(await pending, 42);
  assert.equal(promises, 0);
  assert.equal(currentZLinkActorExecution(), undefined);
});

test('handler Promise assimilation retains the deferred Join registration scope', async () => {
  const {
    runActorHandlerWithDeferredJoins,
    deferActorJoin
  } = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
  const events = [];
  class HandlerPromise extends Promise {
    then(...args) {
      deferActorJoin({
        async execute() {
          events.push('joined');
        },
        discard() {
          events.push('discarded');
        }
      });
      return super.then(...args);
    }
  }
  assert.equal(
    await runActorHandlerWithDeferredJoins(() => new HandlerPromise((resolve) => resolve(42))),
    42
  );
  assert.deepEqual(events, ['joined']);
});

test('Actor fallback examines only handlers for the incoming kind and packet', () => {
  const {
    ZLinkSpotActorHandlerRegistryRuntime,
    ZLinkActorPacketKind
  } = require('../../packages/framework/dist/runtime/actors/spot-actor-dispatch');
  class Parent {}
  class Child extends Parent {}
  class Handler {}
  const registry = new ZLinkSpotActorHandlerRegistryRuntime();
  for (let index = 0; index < 32; index++) {
    registry.addPacket({
      kind: ZLinkActorPacketKind.Request,
      actorType: Parent,
      packetName: `unrelated${index}`,
      handlerType: Handler
    });
  }
  registry.addPacket({
    kind: ZLinkActorPacketKind.Request,
    actorType: Parent,
    packetName: 'packet',
    handlerType: Handler
  });
  const values = Map.prototype.values;
  let visits = 0;
  Map.prototype.values = function () {
    const iterator = values.call(this);
    return {
      [Symbol.iterator]() {
        return this;
      },
      next() {
        const next = iterator.next();
        if (!next.done && next.value?.handlerType === Handler) visits++;
        return next;
      },
      return() {
        return iterator.return?.() ?? { done: true };
      }
    };
  };
  try {
    for (let index = 0; index < 1000; index++) {
      assert.equal(
        registry.resolvePacket(ZLinkActorPacketKind.Request, new Child(), 'packet').handlerType,
        Handler
      );
    }
  } finally {
    Map.prototype.values = values;
  }
  assert.equal(visits, 1000);
});

test('Actor registration preserves exact, wildcard, inherited order and type-name duplicate rules', () => {
  const {
    ZLinkSpotActorHandlerRegistryRuntime,
    ZLinkActorPacketKind
  } = require('../../packages/framework/dist/runtime/actors/spot-actor-dispatch');
  class Parent {}
  class Middle extends Parent {}
  class Child extends Middle {}
  class First {}
  class Second {}
  const registry = new ZLinkSpotActorHandlerRegistryRuntime();
  const add = (
    actorType,
    handlerType,
    packetName = 'packet',
    kind = ZLinkActorPacketKind.Request
  ) => registry.addPacket({ kind, actorType, packetName, handlerType });
  const resolve = () =>
    registry.resolvePacket(ZLinkActorPacketKind.Request, new Child(), 'packet').handlerType;
  add(Parent, First);
  add(Middle, Second);
  assert.equal(resolve(), First, 'Inherited matching retains registration order.');
  add(Object, Second);
  assert.equal(resolve(), Second, 'Wildcard precedes inherited matching.');
  add(Child, First);
  assert.equal(resolve(), First, 'Exact type name precedes wildcard.');
  const SameName = class Child {};
  assert.throws(() => add(SameName, Second), /already registered/);
  assert.doesNotThrow(() => add(SameName, Second, 'another'));
  assert.doesNotThrow(() => add(SameName, Second, 'packet', ZLinkActorPacketKind.Send));
  assert.equal(
    registry.resolvePacket(ZLinkActorPacketKind.Send, new Child(), 'packet').handlerType,
    Second
  );
});
