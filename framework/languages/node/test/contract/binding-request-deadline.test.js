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
    adapter: (onAdmitted) => submitBindingRequest(operation, parts, REQUEST_BUDGET_MS, undefined, onAdmitted),
    router: () => new Promise((resolve, reject) => router.request('target', parts, REQUEST_BUDGET_MS, resolve, reject)),
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

for (const backpressured of [false, true]) {
  test(`public binding request observes physical admission before reply (pending=${backpressured})`, async t => {
    const admitted = deferred();
    const reply = deferred();
    const fixture = requestFixture(t, {
      result: backpressured ? zlink.SubmitResult.Backpressured : zlink.SubmitResult.Ok,
      admitted: admitted.promise,
      reply: reply.promise
    });
    const registration = framework.createFrameworkRegistration({
      spotNodes: { mesh: { router: { bind: 'inproc://binding-admission' } } }
    });
    const client = new framework.DefaultZLinkRouteClient(registration, { request(_mesh, _rid, _packet, _request, _timeout, _signal, _metadata, onAdmitted) { return fixture.adapter(onAdmitted); } });
    const pending = client.requestToNode('mesh', 'peer', {}).submit();
    assert.ok(pending.admitted instanceof Promise);
    let accepted = false;
    let replied = false;
    void pending.admitted.then(() => { accepted = true; });
    void pending.then(() => { replied = true; });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(accepted, !backpressured);
    assert.equal(replied, false);
    admitted.resolve();
    await pending.admitted;
    assert.equal(replied, false);
    reply.resolve([]);
    assert.deepEqual(await pending, []);
  });
}

for (const terminal of ['timeout', 'cancellation']) {
  test(`public pending binding admission and reply share ${terminal} failure`, async t => {
    const admitted = deferred();
    const reply = deferred();
    const fixture = requestFixture(t, {
      result: zlink.SubmitResult.Backpressured,
      admitted: admitted.promise,
      reply: reply.promise
    });
    const { awaitWithAbort } = require('../../packages/framework/dist/runtime/abort');
    const registration = framework.createFrameworkRegistration({
      spotNodes: { mesh: { router: { bind: 'inproc://binding-terminal' } } }
    });
    const controller = new AbortController();
    const client = new framework.DefaultZLinkRouteClient(registration, {
      request(_mesh, _rid, _packet, _request, _timeout, signal, _metadata, onAdmitted) {
        return awaitWithAbort(fixture.adapter(onAdmitted), signal);
      }
    });
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const pending = client.requestToNode('mesh', 'peer', {}).submit(controller.signal);
    assert.ok(pending.admitted instanceof Promise);
    const results = Promise.allSettled([pending.admitted, pending]);
    if (terminal === 'timeout') t.mock.timers.tick(REQUEST_BUDGET_MS);
    else controller.abort(new Error('cancel-before-admission'));
    const stages = await results;
    assert.equal(stages[0].status, 'rejected');
    assert.equal(stages[1].status, 'rejected');
    assert.equal(stages[0].reason, stages[1].reason);
    if (terminal === 'timeout') {
      assert.equal(stages[1].reason.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
    } else {
      assert.equal(stages[1].reason.name, 'AbortError');
    }
    admitted.resolve();
    reply.resolve([]);
  });
}
