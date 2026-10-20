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

test('serial records preserve first settlement', async () => {
  const scheduler = queue();
  const pending = scheduler.submit(() => 42);
  const record = scheduler.selectNext();
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

function countedArray(array) {
  const calls = { iterators: 0, splice: 0 };
  array[Symbol.iterator] = function () {
    calls.iterators++;
    return Array.prototype[Symbol.iterator].call(this);
  };
  array.splice = function (...args) {
    calls.splice++;
    return Array.prototype.splice.apply(this, args);
  };
  return calls;
}
test('idle waiter notification resolves native waiters without snapshot or iterator allocations', async () => {
  const scheduler = queue();
  const calls = countedArray(scheduler.idleWaiters);
  const events = [];
  const first = new Promise((resolve) => scheduler.idleWaiters.push(resolve)).then(() =>
    events.push('first')
  );
  const second = new Promise((resolve) => scheduler.idleWaiters.push(resolve)).then(() =>
    events.push('second')
  );
  scheduler.resolveIdleWaiters();
  scheduler.resolveIdleWaiters();
  await Promise.all([first, second]);
  assert.deepEqual(events, ['first', 'second']);
  assert.equal(scheduler.idleWaiters.length, 0);
  assert.deepEqual(calls, { iterators: 0, splice: 0 });
  await scheduler.close();
});
test('pending application visitation retains live array order without an iterator', async () => {
  const scheduler = queue();
  const pending = [scheduler.submit(() => 1)];
  const calls = countedArray(scheduler.application.records);
  const visited = [];
  scheduler.visitPendingApplication((record) => {
    visited.push(record.acceptedSequence);
    if (visited.length === 1) pending.push(scheduler.submit(() => 2));
  });
  assert.deepEqual(visited, [1n, 2n]);
  assert.equal(calls.iterators, 0);
  assert.deepEqual(await Promise.all(pending), [1, 2]);
  await scheduler.close();
});
test('non-head selection and withdrawal remove one reference without a splice result', async () => {
  for (const withdraw of [false, true]) {
    const scheduler = queue();
    let withdrawal;
    let canceled = 0;
    const first = scheduler.submit(() => 1);
    const second = withdraw
      ? scheduler.admitDurablePrefix(
          () => 2,
          {
            onWithdrawable: (callback) => {
              withdrawal = callback;
            }
          },
          undefined,
          {
            async prepare() {},
            cancel() {
              canceled++;
            }
          }
        )
      : scheduler.submit(() => 2);
    const pending = [first, second, scheduler.submit(() => 3)];
    const records = [...scheduler.application.records];
    const calls = countedArray(scheduler.application.records);
    if (withdraw) {
      assert.equal(withdrawal(), true);
      assert.equal(withdrawal(), false);
    } else {
      assert.equal(scheduler.takeApplication(records[1]), records[1]);
      records[1].resolve(2);
      records[1].release();
    }
    assert.equal(scheduler.application.records.length, 2);
    assert.strictEqual(scheduler.application.records[0], records[0]);
    assert.strictEqual(scheduler.application.records[1], records[2]);
    assert.equal(canceled, withdraw ? 1 : 0);
    assert.equal(calls.splice, 0);
    assert.deepEqual(await Promise.all(pending), [1, withdraw ? undefined : 2, 3]);
    await scheduler.close();
  }
});
test('continuation admission reuses the existing default lane without an options bag', async () => {
  const scheduler = queue();
  const admit = scheduler.admit;
  scheduler.admit = function (operation, options, ...rest) {
    assert.equal(options, undefined);
    return admit.call(this, operation, options, ...rest);
  };
  assert.equal(await scheduler.submitContinuation(() => 42), 42);
  await scheduler.close();
});

const {
  ZLinkSpotActorDispatcher,
  ZLinkSpotActorHandlerRegistryRuntime,
  ZLinkActorPacketKind
} = require('../../packages/framework/dist/runtime/actors/spot-actor-dispatch');
const {
  ZLinkMessageMetadataEmpty
} = require('../../packages/framework/dist/contracts/Common/ZLinkMessageMetadata');
function dispatcher() {
  class Actor {}
  class Handler {
    async handle(_spot, _actor, _context, message) {
      return message;
    }
  }
  const registry = new ZLinkSpotActorHandlerRegistryRuntime();
  registry.addPacket({
    kind: ZLinkActorPacketKind.Request,
    actorType: Actor,
    handlerType: Handler,
    packetName: 'echo'
  });
  return { actor: new Actor(), dispatch: new ZLinkSpotActorDispatcher({ registry, spot: {} }) };
}
test('Actor reply phases reuse the existing empty read-only metadata owner', async () => {
  const { actor, dispatch } = dispatcher();
  let phases = 0;
  const check = (reply, options) => {
    phases++;
    assert.strictEqual(options.metadata, ZLinkMessageMetadataEmpty.values);
    assert.equal(options.metadata.size, 0);
    assert.deepEqual([...options.metadata], []);
    assert.equal(options.compressPayload, false);
    return reply;
  };
  assert.equal(
    await dispatch.dispatchRequestThenDecoded(actor, 'echo', () => 42, undefined, check, check),
    42
  );
  assert.equal(phases, 2);
});
test('Actor dispatch omits the intermediate default partial context object', async () => {
  const { actor, dispatch } = dispatcher();
  const create = dispatch.createContext;
  dispatch.createContext = function (packetName, context) {
    assert.equal(context, undefined);
    return create.call(this, packetName, context);
  };
  assert.equal(await dispatch.dispatchRequest(actor, 'echo', 42), 42);
});
test('deferred Join preparation and execution retain order without array iterators', async () => {
  const {
    runActorHandlerWithDeferredJoins,
    deferActorJoin
  } = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
  const original = Array.prototype[Symbol.iterator];
  let iterations = 0;
  Array.prototype[Symbol.iterator] = function () {
    Array.prototype[Symbol.iterator] = original;
    let stack;
    try {
      stack = new Error().stack;
    } finally {
      Array.prototype[Symbol.iterator] = replacement;
    }
    if (
      stack.includes('ActorJoinRegistrationScope.prepareSealedJoinsCore') ||
      stack.includes('ActorJoinRegistrationScope.execute')
    )
      iterations++;
    return original.call(this);
  };
  const replacement = Array.prototype[Symbol.iterator];
  const events = [];
  try {
    assert.equal(
      await runActorHandlerWithDeferredJoins(
        () => {
          deferActorJoin({
            prepare() {
              events.push('prepare');
            },
            async execute() {
              events.push('execute');
            },
            discard() {
              events.push('discard');
            }
          });
          return Promise.resolve(42);
        },
        (reply) => {
          events.push('reply');
          return reply;
        }
      ),
      42
    );
  } finally {
    Array.prototype[Symbol.iterator] = original;
  }
  assert.deepEqual(events, ['prepare', 'reply', 'execute']);
  assert.equal(iterations, 0);
});

for (const remaining of [0, 1])
  test(`selected-record fence preserves the original error with ${remaining} remaining records`, async () => {
    const queue = new ZLinkSerialExecutionQueue(
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
    const pending = [queue.submit(() => 1)];
    if (remaining) pending.push(queue.submit(() => 2));
    const records = [...queue.application.records];
    const selection = queue.selectNext();
    queue.application.records.shift();
    let failure;
    try {
      queue.takeSelected(selection);
    } catch (error) {
      failure = error;
    }
    for (let index = 0; index < records.length; index++) {
      records[index].resolve(index + 1);
      records[index].release();
    }
    await Promise.all(pending);
    await queue.close();
    assert.match(
      failure?.message ?? '',
      /The selected serial work record changed before owner claim\./
    );
  });

test('channel flow dispatch binds only the existing invoker inside the enabled branch', () => {
  const {
    ZLinkChannelDispatchPipeline
  } = require('../../packages/framework/dist/runtime/channels/channel-dispatch-pipeline');
  for (const enabled of [false, true]) {
    const pipeline = Object.create(ZLinkChannelDispatchPipeline.prototype);
    pipeline.options = { dispatchErrors: { flow: { flowCreationEnabled: () => enabled } } };
    const dispatch = { fields: {} };
    const handler = {};
    let bindings = 0;
    pipeline.invoke = function (record, selectedHandler) {
      assert.equal(this, pipeline);
      assert.equal(record, dispatch);
      assert.equal(selectedHandler, handler);
      return 42;
    };
    pipeline.invoke.bind = function (...args) {
      bindings += 1;
      return Function.prototype.bind.apply(pipeline.invoke, args);
    };
    assert.equal(pipeline.invokeInFlow(dispatch, handler), 42);
    assert.equal(bindings, enabled ? 1 : 0);
  }
});

test('channel invoker preserves record getter and handler flow scopes', async () => {
  const {
    ZLinkChannelDispatchPipeline
  } = require('../../packages/framework/dist/runtime/channels/channel-dispatch-pipeline');
  const {
    currentFlowContext
  } = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
  for (const enabled of [false, true]) {
    const pipeline = Object.create(ZLinkChannelDispatchPipeline.prototype);
    pipeline.options = { dispatchErrors: { flow: { flowCreationEnabled: () => enabled } } };
    pipeline.filters = [];
    pipeline.trace = () => {};
    const scope = () =>
      assert.equal(currentFlowContext()?.flowId, enabled ? 'channel-flow' : undefined);
    const payload = Buffer.from('payload');
    const context = {};
    const record = { fields: { flowId: 'channel-flow', flowOrigin: 'Inbound' } };
    for (const [key, value] of Object.entries({
      envelope: { header: { contentType: 'application/octet-stream' }, payload },
      codecs: undefined,
      context,
      signal: undefined
    })) {
      Object.defineProperty(record, key, {
        get() {
          scope();
          return value;
        }
      });
    }
    const handler = {
      handle(value, activeContext) {
        scope();
        assert.equal(this, handler);
        assert.equal(value, payload);
        assert.equal(activeContext, context);
        return Promise.resolve().then(() => {
          scope();
          return 42;
        });
      }
    };
    assert.equal(await pipeline.invokeInFlow(record, handler), 42);
    assert.equal(currentFlowContext(), undefined);
    const failure = new Error('handler failed');
    handler.handle = () => {
      scope();
      throw failure;
    };
    assert.throws(
      () => pipeline.invokeInFlow(record, handler),
      (error) => error === failure
    );
    handler.handle = () => ({
      then(resolve) {
        resolve(17);
      }
    });
    assert.equal(await pipeline.invokeInFlow(record, handler), 17);
    assert.equal(currentFlowContext(), undefined);
  }
});

test('serial lifecycle selection preserves predicate receiver, one argument and FIFO search order', () => {
  const first = { lane: 'application' };
  const second = { lane: 'application' };
  const visited = [];
  const owner = {
    canExecuteApplication(...args) {
      assert.equal(this, owner);
      assert.equal(args.length, 1);
      visited.push(args[0]);
      return args[0] === second;
    }
  };
  const scheduler = Object.create(ZLinkSerialExecutionQueue.prototype);
  scheduler.application = { records: [first, second] };
  scheduler.lifecycle = { records: [] };
  assert.equal(scheduler.selectNext(), first);
  assert.deepEqual(visited, []);
  scheduler.activeLifecycle = { context: owner };
  assert.equal(scheduler.selectNext(), second);
  assert.deepEqual(visited, [first, second]);
});
