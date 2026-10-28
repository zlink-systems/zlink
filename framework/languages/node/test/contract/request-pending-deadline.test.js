'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  OperationRegistry
} = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const {
  RequestRuntime
} = require('../../packages/framework/dist/runtime/foundation/request-runtime');
const {
  ZLinkMeshCompletionTable
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const {
  waitRequestReply
} = require('../../packages/framework/dist/runtime/messaging/request-deadline');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist/internal');
function deferred() {
  let resolve;
  const promise = new Promise((yes) => {
    resolve = yes;
  });
  return { promise, resolve };
}
function submit(registry, table, signal) {
  const operation = registry.register(60_000);
  const promise = table.submit(
    () => {
      void operation.promise.then(
        (value) =>
          table.complete({
            operationId: { high: 1n, low: operation.id },
            terminalResult: value,
            failureErrno: 0,
            operationKind: 1,
            kindData: null,
            parts: []
          }),
        () => {}
      );
      return { high: 1n, low: operation.id };
    },
    signal,
    operation
  );
  return { ...operation, promise };
}
test('caller cancellation takes the same explicitly supplied entry immediately', async () => {
  const registry = new OperationRegistry();
  const table = new ZLinkMeshCompletionTable();
  const controller = new AbortController();
  const pending = submit(registry, table, controller.signal);
  const cancelled = assert.rejects(pending.promise, { name: 'AbortError' });
  controller.abort();
  assert.equal(registry.size, 0);
  assert.equal(registry.nextDeadlineMs(), Number.POSITIVE_INFINITY);
  assert.throws(() => registry.handle(pending.id), /has no active registry entry/);
  assert.equal(registry.complete(pending.id, 0), false);
  await cancelled;
  table.dispose();
  registry.close();
});
test('reply take wins cancellation before dispatcher delivery', async () => {
  const registry = new OperationRegistry();
  const table = new ZLinkMeshCompletionTable();
  const controller = new AbortController();
  const pending = submit(registry, table, controller.signal);
  registry.complete(pending.id, 0);
  controller.abort();
  assert.equal((await pending.promise).terminalResult, 0);
  assert.equal(table.pendingCount, 0);
  registry.close();
  table.dispose();
});
test('1000 requests of one runtime share a management timer', async (t) => {
  const owner = new RequestRuntime();
  const source = deferred();
  const timer = t.mock.method(globalThis, 'setTimeout');
  const requests = Array.from({ length: 1000 }, () =>
    owner.request(60_000, undefined, (pending) =>
      waitRequestReply(source.promise, 'admission', pending)
    )
  );
  assert.equal(owner.registry.size, 1000);
  const created = timer.mock.callCount();
  source.resolve('reply');
  await Promise.all(requests);
  assert.ok(created <= 1, `management timer creations: ${created}`);
  assert.equal(owner.registry.size, 0);
  t.diagnostic(`1000 concurrent requests: timer creations=${created}`);
  owner.close();
});
test('reply removes every deadline entry reference immediately', async () => {
  const registry = new OperationRegistry();
  const pending = Array.from({ length: 1000 }, () => registry.register(60_000));
  for (const operation of pending) operation.complete('reply');
  await Promise.all(pending.map((operation) => operation.promise));
  assert.equal(registry.nextDeadlineMs(), Number.POSITIVE_INFINITY);
  for (const operation of pending) {
    assert.throws(() => registry.handle(operation.id), /has no active registry entry/);
  }
  registry.close();
});
test('overdue request management visit is not deferred by a fixed lower bound', async (t) => {
  let timeReads = 0;
  t.mock.method(performance, 'now', () => (timeReads++ < 2 ? 0 : 10));
  const timer = t.mock.method(globalThis, 'setTimeout', () => ({
    ref() {},
    unref() {},
    hasRef() { return true; }
  }));
  const owner = new RequestRuntime();
  const pending = owner.begin(0);
  const rejected = assert.rejects(pending.promise);
  assert.ok(timer.mock.calls[0].arguments[1] <= 0);
  owner.close();
  await rejected;
});
test('1000 deadline expirations use bounded owner visits', async (t) => {
  const owner = new RequestRuntime();
  const expire = OperationRegistry.prototype.expire;
  let visits = 0;
  t.mock.method(OperationRegistry.prototype, 'expire', function (...args) {
    visits++;
    return expire.apply(this, args);
  });
  const pending = Array.from({ length: 1000 }, () =>
    owner
      .request(40, undefined, (entry) => waitRequestReply(new Promise(() => {}), 'HWM', entry))
      .catch((error) => error)
  );
  const errors = await Promise.all(pending);
  assert.ok(errors.every((error) => error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded));
  assert.ok(visits > 0 && visits < 32, `management visits: ${visits}`);
  t.diagnostic(`1000 expirations: management visits=${visits}`);
  owner.close();
});
for (const admittedBeforeDeadline of [false, true])
  test(`readiness and HWM register once (admitted=${admittedBeforeDeadline})`, async (t) => {
    const {
      bindingRequestReply
    } = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
    const { SubmitResult } = require('@zlink-systems/zlink');
    const owner = new RequestRuntime();
    const register = t.mock.method(OperationRegistry.prototype, 'register');
    const admitted = deferred();
    const reply = deferred();
    let seen;
    let closed = 0;
    const request = owner.request(60, undefined, async (pending) => {
      seen = pending;
      await waitRequestReply(
        new Promise((resolve) => setTimeout(resolve, 20)),
        'readiness',
        pending
      );
      assert.equal(pending.isPending(), true, 'readiness must not complete the entry');
      if (admittedBeforeDeadline) admitted.resolve();
      return waitRequestReply(
        bindingRequestReply({
          result: SubmitResult.Backpressured,
          admitted: admitted.promise,
          reply: reply.promise
        }),
        'HWM',
        pending,
        undefined,
        (parts) => {
          for (const part of parts) part.close();
        }
      );
    });
    await assert.rejects(
      request,
      (error) => error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
    );
    assert.equal(register.mock.callCount(), 1);
    assert.equal(owner.registry.size, 0);
    assert.equal(seen.isPending(), false);
    reply.resolve([
      {
        close() {
          closed++;
        }
      }
    ]);
    admitted.resolve();
    await new Promise(setImmediate);
    assert.equal(closed, 1);
    owner.close();
  });
test('binding consumes the supplied entry across async admission without registering', async (t) => {
  const {
    submitBindingRequest
  } = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
  const { SubmitResult } = require('@zlink-systems/zlink');
  const owner = new RequestRuntime();
  const controller = new AbortController();
  const admitted = deferred();
  const reply = deferred();
  const register = t.mock.method(OperationRegistry.prototype, 'register');
  const native = {
    message() {
      return this;
    },
    timeout() {
      return this;
    },
    submit() {
      return {
        result: SubmitResult.Backpressured,
        admitted: admitted.promise,
        reply: reply.promise
      };
    }
  };
  const pending = owner.request(60_000, controller.signal, (entry) =>
    submitBindingRequest(native, Buffer.from('request'), 60_000, undefined, undefined, entry)
  );
  const cancelled = assert.rejects(pending, { name: 'AbortError' });
  controller.abort();
  await cancelled;
  assert.equal(register.mock.callCount(), 1);
  assert.equal(owner.registry.size, 0);
  admitted.resolve();
  reply.resolve([]);
  await new Promise(setImmediate);
  owner.close();
});

test('caller deadline takes an entry while work awaits an unwrapped promise', async (t) => {
  t.mock.timers.enable({ apis: ['Date', 'setTimeout'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
  const owner = new RequestRuntime();
  const work = deferred();
  let terminal;
  const caller = owner.request(25, undefined, async () => await work.promise);
  const observed = caller.then(
    (value) => {
      terminal = value;
    },
    (error) => {
      terminal = error;
    }
  );
  try {
    t.mock.timers.tick(25);
    await new Promise(setImmediate);
    assert.equal(terminal?.kind, ZLinkFrameworkErrorKind.DeadlineExceeded);
    assert.equal(owner.registry.size, 0);
  } finally {
    work.resolve('late');
    await observed;
    owner.close();
  }
});

test('registered handles share prototype methods and are the registry entry', async () => {
  const registry = new OperationRegistry();
  const pending = registry.register(1000);
  const observedId = registry.registerObserved(1000, {
    operationCompleted() {},
    operationFailed() {}
  });
  const observed = registry.handle(observedId);
  assert.equal(registry.handle(pending.id), pending);
  assert.equal(registry.handle(observedId), observed);
  for (const method of [
    'complete',
    'fail',
    'takeFailure',
    'isPending',
    'setFailureDispatcher',
    'clearFailureDispatcher'
  ]) {
    assert.equal(Object.hasOwn(pending, method), false);
    assert.equal(pending[method], observed[method]);
  }
  registry.complete(pending.id, 'reply');
  registry.complete(observedId, 'reply');
  assert.equal(await pending.promise, 'reply');
});

for (const terminal of ['cancel', 'close'])
  test(`entry caller ends unwrapped work on ${terminal} and disposes its late result`, async () => {
    const owner = new RequestRuntime();
    const controller = new AbortController();
    const pending = owner.begin(1000, controller.signal);
    const work = deferred();
    let closes = 0;
    const caller = owner.run(
      pending,
      () => work.promise,
      (value) => value.close()
    );
    assert.equal(caller, pending.promise);
    const reason = new Error(terminal);
    const failed = assert.rejects(caller, (error) => error === reason);
    if (terminal === 'cancel') controller.abort(reason);
    else owner.close(reason);
    await failed;
    work.resolve({
      close() {
        closes++;
      }
    });
    await new Promise(setImmediate);
    assert.equal(closes, 1);
    assert.equal(owner.registry.size, 0);
    owner.close();
  });

test('native stage notification leaves the caller entry pending until work completes', async (t) => {
  t.mock.timers.enable({ apis: ['Date', 'setTimeout'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
  const owner = new RequestRuntime();
  const work = deferred();
  const caller = owner.request(25, undefined, async (entry) => {
    entry.reply('native stage');
    await work.promise;
    return 'decoded reply';
  });
  assert.equal(owner.registry.size, 1);
  const terminal = assert.rejects(caller, { kind: ZLinkFrameworkErrorKind.DeadlineExceeded });
  t.mock.timers.tick(25);
  await terminal;
  work.resolve();
  await new Promise(setImmediate);
  assert.equal(owner.registry.size, 0);
  owner.close();
});

test('caller close disposes a stage result already queued for work', async () => {
  const owner = new RequestRuntime();
  const stage = deferred();
  let closes = 0;
  const pending = owner.begin(1000);
  const caller = owner.run(
    pending,
    async () => await waitRequestReply(stage.promise, 'stage', pending),
    (parts) => {
      for (const part of parts) part.close();
    }
  );
  const reason = new Error('closed after stage notification');
  const failed = assert.rejects(caller, (error) => error === reason);
  stage.resolve([
    {
      close() {
        closes++;
      }
    }
  ]);
  await Promise.resolve();
  owner.close(reason);
  await failed;
  await new Promise(setImmediate);
  assert.equal(closes, 1);
});
