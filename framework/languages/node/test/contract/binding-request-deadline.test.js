'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const {
  submitBindingRequest
} = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
const {
  ZLinkNodeRawBindingPort
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const framework = require('../../packages/framework/dist/internal');

const REQUEST_BUDGET_MS = 50;

function deferred() {
  let resolve;
  const promise = new Promise(yes => { resolve = yes; });
  return { promise, resolve };
}

function requestFixture(t, submission) {
  const operation = {
    message() { return this; },
    timeout(value) {
      assert.equal(value, REQUEST_BUDGET_MS);
      return this;
    },
    submit() { return submission; }
  };
  const context = zlink.createContext();
  for (const factory of ['createRouterSocket', 'createDealerSocket']) {
    const createSocket = zlink[factory];
    t.mock.method(zlink, factory, value => {
      const socket = createSocket(value);
      t.mock.method(socket, 'request', () => operation);
      return socket;
    });
  }
  const host = new ZLinkNodeRawBindingPort(context).createHost();
  const router = host.createRouter();
  const dealer = host.createDealer();
  t.after(() => { host.close(); context.close(); });
  const parts = [Buffer.from('request')];
  return {
    adapter: () => submitBindingRequest(operation, parts, REQUEST_BUDGET_MS),
    router: () => router.request('target', parts, REQUEST_BUDGET_MS),
    dealer: () => dealer.request(parts, REQUEST_BUDGET_MS)
  };
}

for (const path of ['adapter', 'router', 'dealer']) {
  test(`${path} immediately admitted request creates no Framework timer`, async t => {
    const fixture = requestFixture(t, {
      result: zlink.SubmitResult.Ok,
      admitted: new Promise(() => {}),
      reply: Promise.resolve([])
    });
    const timer = t.mock.method(globalThis, 'setTimeout');
    assert.deepEqual(await fixture[path](), []);
    assert.equal(timer.mock.callCount(), 0);
  });

  for (const admitBeforeDeadline of [false, true]) {
    test(`${path} pending request expires and closes late reply (admitted early=${admitBeforeDeadline})`, async t => {
      const admitted = deferred();
      const reply = deferred();
      const fixture = requestFixture(t, {
        result: zlink.SubmitResult.Backpressured,
        admitted: admitted.promise,
        reply: reply.promise
      });
      t.mock.timers.enable({ apis: ['setTimeout'] });
      const timer = t.mock.method(globalThis, 'setTimeout');
      const pending = fixture[path]();
      const terminal = assert.rejects(pending,
        error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      assert.equal(timer.mock.callCount(), 1);
      if (admitBeforeDeadline) {
        admitted.resolve();
        await Promise.resolve();
      }
      t.mock.timers.tick(REQUEST_BUDGET_MS);
      await terminal;
      assert.equal(timer.mock.callCount(), 1);
      let closes = 0;
      reply.resolve([{ close() { closes += 1; } }]);
      admitted.resolve();
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(closes, 1);
    });
  }
}
