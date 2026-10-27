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
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
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
  assert.equal(registry.entries.size, 0);
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
  assert.equal(registry.entries.size, 0);
  registry.close();
});
test('overdue management visit is not deferred by a fixed lower bound', (t) => {
  const now = performance.now();
  t.mock.method(performance, 'now', () => now);
  const backend = Object.create(ZLinkNodeRawMeshBackend.prototype);
  backend.onMaintenance = () => {};
  backend.runtime = { nextMaintenanceDeadlineMs: () => now - 10, pendingOperationCount: 1 };
  const timer = t.mock.method(globalThis, 'setTimeout');
  backend.scheduleMaintenance();
  try {
    assert.equal(backend.maintenanceDueMs, now - 10);
    assert.ok(timer.mock.calls[0].arguments[1] <= 0);
  } finally {
    clearTimeout(backend.maintenanceTimer);
  }
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
