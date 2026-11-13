'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const slab = require('../../packages/framework/dist/contracts/Common/payload-slab');
const { ZLinkNodeRawBindingPort } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const { admitServiceRequest } = require('../../packages/framework/dist/runtime/foundation/durable-service-request');
const { OperationRegistry } = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const { fixture } = require('./fixtures/raw-mesh-close');
const { frameworkPair } = require('../../perf/request-backpressure.cjs');
const PACKET = 'ZLinkFrameworkMultipart';
const CONTENT = 'application/x-zlink-multipart';

function expectedFrame(parts) {
  const u32 = n => { const bytes = Buffer.alloc(4); bytes.writeUInt32BE(n); return bytes; };
  const text8 = s => Buffer.concat([Buffer.from([Buffer.byteLength(s)]), Buffer.from(s)]);
  const encoded = parts.map(p => typeof p === 'string' ? Buffer.from(p) : p);
  const multipart = Buffer.concat([u32(parts.length), ...encoded.flatMap(p => [u32(p.length), p])]);
  const body = Buffer.concat([text8(PACKET), text8(CONTENT), u32(multipart.length), multipart]);
  return Buffer.concat([Buffer.from([1]), u32(body.length), body]);
}

for (const size of [0, 1, 1024, 4096, 70000]) {
  test(`native service frame preserves independent wire bytes and snapshot for ${size} bytes`, t => {
    const backing = new Uint8Array(size + 2).fill(0xee);
    const body = backing.subarray(1, size + 1);
    for (let i = 0; i < size; i++) body[i] = i & 255;
    const parts = [Buffer.from([0, 255]), '', '한😀', '\ud800', body];
    const expected = expectedFrame(parts);
    const allocations = t.mock.method(slab, 'allocatePayloadBytes');
    const allocate = t.mock.method(zlink.Message, 'allocate');
    const copies = t.mock.method(Buffer.prototype, 'set');
    const from = t.mock.method(Buffer, 'from');
    const managedAllocations = ['alloc', 'allocUnsafe', 'allocUnsafeSlow'].map(name =>
      t.mock.method(Buffer, name));
    const frame = wire.encodeFrameworkMultipartApplicationMessage(parts);
    try {
      assert.equal(allocations.mock.callCount(), 0, 'no intermediate managed application frame');
      assert.equal(allocate.mock.callCount(), 1);
      assert.equal(allocate.mock.calls[0].arguments[0], expected.length);
      const native = allocate.mock.calls[0].result;
      assert.equal(from.mock.callCount(), 0, 'no intermediate copied Buffer');
      assert.ok(managedAllocations.every(method => method.mock.callCount() === 0), 'no managed frame allocation');
      assert.equal(copies.mock.callCount(), 3, 'one fixed header copy and one write per binary part');
      for (const call of copies.mock.calls) assert.strictEqual(call.this, native.data());
      assert.equal(copies.mock.calls.filter(call => call.arguments[0] === body).length, 1);
      assert.equal(copies.mock.calls.filter(call => call.arguments[0] === parts[0]).length, 1);
      assert.strictEqual(frame.data(), native.data(), 'writer exposes the final native storage');
      assert.deepEqual(frame.data(), expected);
      assert.deepEqual(wire.encodeMultipartApplicationPayload(parts, PACKET, CONTENT), expected);
      const decoded = wire.decodeMultipartApplicationParts(frame.data(), part => part);
      assert.equal(decoded.length, parts.length);
      assert.strictEqual(decoded[4].buffer, frame.data().buffer, 'receive decoder retains final part views');
      body.fill(0x33);
      parts[0].fill(0x44);
      assert.deepEqual(frame.data(), expected, 'encoding snapshots caller bytes before submission');
      const bad = Buffer.from(expected); bad[4] ^= 1;
      assert.throws(() => assert.deepEqual(frame.data(), bad), assert.AssertionError);
      assert.throws(() => wire.decodeMultipartApplicationParts(bad, part => part), wire.ServiceWireProtocolError);
    } finally { frame.close(); }
    assert.throws(() => frame.data(), /ownership has ended/);
  });
}

test('native service encoder validates before allocation and releases a failed write', t => {
  const allocate = t.mock.method(zlink.Message, 'allocate');
  for (const parts of [[], [{ byteLength: 0x100000000 }], [{ byteLength: 0xfffffff5 }]]) {
    assert.throws(() => wire.encodeFrameworkMultipartApplicationMessage(parts), wire.ServiceWireProtocolError);
  }
  for (const [packet, content] of [['', CONTENT], ['\ud800', CONTENT], [PACKET, 'x\0y']]) {
    assert.throws(() => wire.encodeMultipartApplicationPayload([Buffer.alloc(0)], packet, content), wire.ServiceWireProtocolError);
  }
  assert.equal(allocate.mock.callCount(), 0);
  const close = t.mock.method(zlink.Message.prototype, 'close');
  const failure = new Error('writer failure');
  const set = t.mock.method(Buffer.prototype, 'set', () => { throw failure; });
  assert.throws(() => wire.encodeFrameworkMultipartApplicationMessage([Buffer.alloc(1)]), error => error === failure);
  set.mock.restore();
  assert.equal(allocate.mock.callCount(), 1);
  assert.equal(close.mock.callCount(), 1);
  assert.strictEqual(close.mock.calls[0].this, allocate.mock.calls[0].result);
});

test('native frame transfers its sole owner once and cannot close the transferred Message', () => {
  const frame = wire.encodeFrameworkMultipartApplicationMessage([Buffer.from('body')]);
  const expected = Buffer.from(frame.data());
  const native = frame.takeMessage();
  try {
    frame.close();
    assert.throws(() => frame.takeMessage(), /ownership has ended/);
    assert.throws(() => frame.data(), /ownership has ended/);
    assert.deepEqual(native.data(), expected);
  } finally { native.close(); }
});

function rawRouter(t) {
  const host = new ZLinkNodeRawBindingPort().createHost();
  const router = host.createRouter();
  router.setRoutingId('service-frame-source');
  t.after(() => host.close());
  return router;
}

for (const result of [zlink.SubmitResult.Ok, zlink.SubmitResult.Backpressured]) {
  test(`raw SEND passes the final native Message to the binding: result ${result}`, async t => {
    const router = rawRouter(t);
    let admit;
    const admitted = new Promise(resolve => { admit = resolve; });
    const receivedParts = [];
    const operation = {
      message(part) { receivedParts.push(part); return this; },
      submit() { return { result, admitted }; }
    };
    t.mock.method(router.socket, 'send', () => operation);
    const frame = wire.encodeFrameworkMultipartApplicationMessage([Buffer.alloc(4096, 7)]);
    const bytes = frame.data();
    const submission = router.submitSend('target', [wire.encodeNodeSendHeader(), frame]);
    assert.equal(submission.result, result);
    const native = receivedParts[1];
    try {
      assert.ok(native instanceof zlink.Message, 'binding must receive native ownership, not Buffer bytes');
      assert.strictEqual(native.data(), bytes, 'submit does not build a second application frame');
      assert.throws(() => frame.data(), /ownership has ended/);
      frame.close();
      assert.equal(native.size(), bytes.length, 'closing detached owner does not release binding payload');
      // The unchanged Buffer encoder is a negative control for the ownership check.
      const legacy = wire.encodeMultipartApplicationPayload([Buffer.alloc(4096, 7)], PACKET, CONTENT);
      assert.throws(() => assert.ok(legacy instanceof zlink.Message), assert.AssertionError);
      admit();
      await submission.admitted;
    } finally { native.close(); }
  });
}

for (const operationKind of ['send', 'request']) {
  test(`raw ${operationKind} synchronous failure releases the detached native frame`, t => {
    const router = rawRouter(t);
    const failure = new Error('submit refused');
    const operation = {
      message() { return this; }, timeout() { return this; }, submit() { throw failure; }
    };
    t.mock.method(router.socket, operationKind, () => operation);
    const frame = wire.encodeFrameworkMultipartApplicationMessage([Buffer.alloc(1024, 1)]);
    const close = t.mock.method(zlink.Message.prototype, 'close');
    if (operationKind === 'send') {
      assert.throws(() => router.submitSend('target', [wire.encodeNodeSendHeader(), frame]), error => error === failure);
    } else {
      assert.throws(() => router.request('target', [wire.encodeNodeRequestHeader(1n), frame], 1000,
        () => {}, () => {}, undefined, undefined, { id: 1n, deadlineMs: performance.now() + 1000 }), error => error === failure);
    }
    assert.equal(close.mock.callCount(), 1);
    assert.throws(() => frame.data(), /ownership has ended/);
    frame.close();
    assert.equal(close.mock.callCount(), 1);
  });
}

for (const observed of [false, true]) {
  test(`deferred request releases an unsubmitted native snapshot on terminal: observed=${observed}`, async () => {
    const registry = new OperationRegistry();
    const error = new Error('cancelled before route readiness');
    const observer = { operationCompleted() {}, operationFailed() {} };
    const pending = observed ? registry.handle(registry.registerObserved(1000, observer)) : registry.register(1000);
    if (pending.promise) pending.promise.catch(() => {});
    const frame = wire.encodeFrameworkMultipartApplicationMessage([Buffer.alloc(4096, 7)]);
    let releases = 0;
    const port = {
      observePeerRoute: () => ({ ready: false, selected: true }),
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved: () => () => {}
    };
    admitServiceRequest(port, 'target', pending, () => assert.fail('unready route submitted'),
      () => {}, 'Request', undefined, () => { releases++; frame.close(); });
    pending.fail(error);
    await Promise.resolve();
    assert.equal(releases, 1);
    assert.throws(() => frame.data(), /ownership has ended/);
    registry.close();
  });
}

test('production typed request/reply uses native frames in both directions', async t => {
  const pair = await frameworkPair();
  try {
    const allocate = t.mock.method(zlink.Message, 'allocate');
    for (let i = 0; i < 3; i++) pair.validate(await pair.submit().reply);
    assert.equal(allocate.mock.callCount(), 6, 'one final service frame per request and reply');
  } finally { await pair.close(); }
});

for (const destination of ['node', 'channel']) {
  test(`production ${destination} send releases a frame when no target accepts it`, async t => {
    const f = fixture();
    try {
      const allocate = t.mock.method(zlink.Message, 'allocate');
      const close = t.mock.method(zlink.Message.prototype, 'close');
      const result = destination === 'node'
        ? await f.backend.sendToNode('absent', [Buffer.alloc(4096, 7)])
        : await f.backend.sendToChannel('absent', [Buffer.alloc(4096, 7)]);
      assert.equal(result, destination === 'node' ? zlink.SubmitResult.NotConnected : zlink.SubmitResult.NotFound);
      assert.equal(allocate.mock.callCount(), 1);
      assert.equal(close.mock.callCount(), 1);
      assert.strictEqual(close.mock.calls[0].this, allocate.mock.calls[0].result);
    } finally { await f.backend.close(); }
  });
}

test('production request failure before submit and local terminal replies release their frame', async () => {
  const f = fixture();
  try {
    const request = wire.encodeFrameworkMultipartApplicationMessage([Buffer.from('request')]);
    const pending = f.runtime.requestToNode('absent', request, 1000);
    assert.equal((await pending.promise).terminalResult, zlink.RequestResult.NotFound);
    assert.throws(() => request.data(), /ownership has ended/);
    for (const failure of [false, true]) {
      const reply = wire.encodeFrameworkMultipartApplicationMessage([Buffer.from('reply')]);
      f.runtime.reply({ localReply(result, code, payload) {
        assert.equal(result, failure ? zlink.RequestResult.InternalError : zlink.RequestResult.Ok);
        assert.equal(payload === undefined, failure);
      } }, reply, failure ? zlink.RequestResult.InternalError : zlink.RequestResult.Ok);
      assert.throws(() => reply.data(), /ownership has ended/);
    }
    const bad = wire.encodeFrameworkMultipartApplicationMessage([Buffer.from('reply')]);
    assert.throws(() => f.runtime.reply({ reply() {}, correlation: 0n }, bad), wire.ServiceWireProtocolError);
    assert.throws(() => bad.data(), /ownership has ended/);
  } finally { await f.backend.close(); }
});


test('a local native frame transfers its retained Buffer without a copy', t => {
  const frame = wire.encodeFrameworkMultipartApplicationMessage([Buffer.alloc(4096, 7)]);
  const original = frame.data();
  const close = t.mock.method(zlink.Message.prototype, 'close');
  const bytes = frame.takeData();
  assert.strictEqual(bytes, original);
  assert.equal(close.mock.callCount(), 1);
  assert.equal(bytes[bytes.length - 1], 7);
  assert.throws(() => frame.data(), /ownership has ended/);
  frame.close();
  assert.equal(close.mock.callCount(), 1);
});
