const assert = require('node:assert/strict');
const { getEventListeners } = require('node:events');
const test = require('node:test');
const {
  OperationRegistry,
  OperationTimeoutError
} = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const {
  RequestRuntime
} = require('../../packages/framework/dist/runtime/foundation/request-runtime');
const {
  ZLinkHostSpotAddressTransport
} = require('../../packages/framework/dist/runtime/host/spot-address-transport');
const {
  prepareZLinkRequestReply,
  identityRequestReply
} = require('../../packages/framework/dist/runtime/messaging/request-result');
const {
  internalFrameworkErrorKind,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

// A missing reason makes Node allocate its default DOMException inside abort().
function countDefaultReasons(t) {
  const original = AbortController.prototype.abort;
  let count = 0;
  t.mock.method(AbortController.prototype, 'abort', function (reason) {
    const pending = !this.signal.aborted;
    original.call(this, reason);
    if (pending && reason === undefined && this.signal.reason instanceof DOMException) count++;
  });
  return () => count;
}

function addressFixture(resolve) {
  const requests = new RequestRuntime();
  const transport = new ZLinkHostSpotAddressTransport({
    requests,
    resolver: () => ({ resolve }),
    routed: {
      requestToSpot: () => prepareZLinkRequestReply(Promise.resolve('reply'), identityRequestReply)
    },
    meshNames: () => [],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 60_000
  });
  return { requests, transport };
}

test('40 successful operations allocate no default abort reasons and release parent listeners', async (t) => {
  const count = countDefaultReasons(t);
  const registry = new OperationRegistry();
  const reasons = new Set();
  const allocations = [];
  for (let index = 0; index < 40; index++) {
    const parent = new AbortController();
    const pending = registry.register(60_000);
    pending.setSignal(parent.signal);
    const signal = pending.signal;
    assert.equal(getEventListeners(parent.signal, 'abort').length, 1);
    const before = count();
    assert.equal(pending.complete('reply'), true);
    allocations.push(count() - before);
    assert.equal(await pending.promise, 'reply');
    assert.equal(signal.aborted, true);
    assert.ok(signal.reason instanceof DOMException);
    assert.equal(signal.reason.name, 'AbortError');
    assert.equal(signal.reason.message, 'This operation was aborted');
    reasons.add(signal.reason);
    assert.equal(getEventListeners(parent.signal, 'abort').length, 0);
    parent.abort('late cancellation');
    assert.equal(await pending.promise, 'reply');
  }
  t.diagnostic(
    `registry: iterations=40 defaultDOMExceptions=${JSON.stringify(allocations)} parentListeners=0 uniqueCleanupReasons=${reasons.size}`
  );
  assert.deepEqual(allocations, Array(40).fill(0));
  assert.equal(reasons.size, 1);
});

test('40 successful Spot address requests release deadline and parent listeners without default reasons', async (t) => {
  const count = countDefaultReasons(t);
  const signals = [];
  const fixture = addressFixture(async (_spotId, signal) => {
    signals.push(signal);
    return { spotId: 'spot', routerChannelId: 'play', targetNodeRid: 'node' };
  });
  const allocations = [];
  for (let index = 0; index < 40; index++) {
    const parent = new AbortController();
    const before = count();
    assert.equal(
      await fixture.transport.requestToSpotAddress('spot', {}, { signal: parent.signal }),
      'reply'
    );
    allocations.push(count() - before);
    assert.equal(signals[index].aborted, true);
    assert.equal(signals[index].reason.name, 'AbortError');
    assert.equal(getEventListeners(parent.signal, 'abort').length, 0);
  }
  t.diagnostic(
    `Spot address: iterations=40 defaultDOMExceptions=${JSON.stringify(allocations)} parentListeners=0`
  );
  assert.deepEqual(allocations, Array(40).fill(0));
  assert.equal(new Set(signals.map((signal) => signal.reason)).size, 1);
});

test('40 registry cancellations and timeouts preserve terminal reasons and release listeners', async () => {
  for (let index = 0; index < 40; index++) {
    const registry = new OperationRegistry({ now: () => 0 });
    for (const preCancelled of [false, true]) {
      const parent = new AbortController();
      const reason = new Error('parent cancellation');
      if (preCancelled) parent.abort(reason);
      const pending = registry.register(10);
      pending.setSignal(parent.signal);
      const rejected = assert.rejects(pending.promise, (error) => error === reason);
      parent.abort(reason);
      await rejected;
      assert.equal(parent.signal.reason, reason);
      assert.equal(getEventListeners(parent.signal, 'abort').length, 0);
    }
    const parent = new AbortController();
    const pending = registry.register(10);
    pending.setSignal(parent.signal);
    const rejected = assert.rejects(
      pending.promise,
      (error) => error instanceof OperationTimeoutError && error.operationId === pending.id
    );
    assert.equal(registry.expire(10, Infinity), 1);
    await rejected;
    assert.equal(getEventListeners(parent.signal, 'abort').length, 0);
    assert.equal(parent.signal.aborted, false);
  }
});

test('40 Spot address cancellations and timeouts preserve parent reason and deadline classification', async () => {
  for (let index = 0; index < 40; index++) {
    for (const timeout of [false, true]) {
      const parent = new AbortController();
      let local;
      const fixture = addressFixture((_spotId, signal) => {
        local = signal;
        return new Promise(() => {});
      });
      const reason = new Error('parent cancellation');
      const reply = fixture.transport.requestToSpotAddress('spot', {}, { signal: parent.signal });
      const rejected = assert.rejects(reply, (error) =>
        timeout
          ? internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded
          : error === reason
      );
      if (timeout) fixture.requests.registry.expire(Number.MAX_SAFE_INTEGER, Infinity);
      else parent.abort(reason);
      await rejected;
      assert.equal(local.aborted, true);
      // The registry's parent listener completes the request first; its cleanup
      // closes the deadline before the deadline's parent listener can run.
      assert.ok(local.reason instanceof DOMException);
      assert.equal(local.reason.name, 'AbortError');
      if (!timeout) assert.equal(parent.signal.reason, reason);
      assert.equal(getEventListeners(parent.signal, 'abort').length, 0);
    }
  }
});
