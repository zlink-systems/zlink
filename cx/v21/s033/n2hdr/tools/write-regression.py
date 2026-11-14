from pathlib import Path
p=Path('/root/worktree/zlink-v21-s033/n2hdr/framework/languages/node/test/contract/node-channel-header-native.test.js')
p.write_text('''\
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { ZLinkNodeRawBindingPort } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const { runWithFlow } = require('../../packages/framework/dist/runtime/diagnostics/flow-context');

const correlation = '00112233445566778899aabbccddeeff';
const payload = Buffer.from('payload');
const flow = { flowId: '01920000-0000-7abc-8def-0123456789ab', flowOrigin: 'Application' };
const bytes = part => Buffer.isBuffer(part) ? part : part.data();
function request(codecs, id = correlation, deadline = 2000000000000, metadata, withFlow = false) {
  return runWithFlow(withFlow ? flow : undefined, () =>
    envelope.encodeChannelEnvelopePartsAtDeadline(1, 'native-header', 'Packet', payload,
      deadline, undefined, codecs, id, withFlow, metadata));
}
function requestHeader(parts) {
  return envelope.decodeChannelHeader([{ data: () => bytes(parts[0]) }], true);
}

for (const prepared of [false, true]) {
  test(`channel header writes into final native storage: prepared=${prepared}`, t => {
    const codecs = prepared ? { serializers: new Map() } : undefined;
    const warm = request(codecs);
    const header = requestHeader(warm);
    envelope.closeMessages(warm);
    envelope.closeMessages(envelope.encodeChannelReplyParts(header, payload, codecs));
    const allocate = t.mock.method(zlink.Message, 'allocate');
    const managed = ['alloc', 'allocUnsafe', 'allocUnsafeSlow'].map(name => t.mock.method(Buffer, name));
    const copy = t.mock.method(Buffer.prototype, 'copy');
    const set = t.mock.method(Buffer.prototype, 'set');
    for (const encode of [() => request(codecs), () => envelope.encodeChannelReplyParts(header, payload, codecs)]) {
      const parts = encode();
      try {
        assert.ok(parts[0] instanceof zlink.Message, 'header must be the final native Message');
        assert.strictEqual(parts[0], allocate.mock.calls.at(-1).result);
        assert.strictEqual(parts[0].data(), allocate.mock.calls.at(-1).result.data());
        assert.strictEqual(parts[1], payload);
      } finally { envelope.closeMessages(parts); }
    }
    assert.equal(allocate.mock.callCount(), 2, 'one final frame per request/reply');
    assert.ok(managed.every(method => method.mock.callCount() === 0), 'no intermediate header Buffer');
    assert.equal(copy.mock.callCount(), 0, 'no template copy into a JS header Buffer');
    assert.equal(set.mock.callCount(), prepared ? 2 : 0);
    for (const call of set.mock.calls) {
      assert.ok(allocate.mock.calls.some(a => a.result.data() === call.this), 'template goes directly into native storage');
    }
  });
}

function routerFixture(t) {
  const host = new ZLinkNodeRawBindingPort().createHost();
  t.after(() => host.close());
  const router = host.createRouter();
  router.setRoutingId('native-channel-header');
  return router;
}

for (const kind of ['send', 'request', 'reply']) {
  for (const fail of [false, true]) {
    test(`channel ${kind} ${fail ? 'failure releases once' : 'success transfers storage'}`, t => {
      const router = routerFixture(t);
      const original = request(undefined);
      const parts = kind === 'reply'
        ? envelope.encodeChannelReplyParts(requestHeader(original), payload)
        : original;
      if (kind === 'reply') envelope.closeMessages(original);
      const native = parts[0];
      const data = bytes(native);
      const failure = new Error('synchronous submit failure');
      const seen = [];
      const operation = {
        message(part) { seen.push(part); return this; },
        timeout() { return this; },
        submit() {
          if (fail) throw failure;
          return { result: zlink.SubmitResult.Ok, admitted: Promise.resolve(), reply: new Promise(() => {}) };
        }
      };
      t.mock.method(router.socket, kind, () => operation);
      const close = t.mock.method(zlink.Message.prototype, 'close');
      const submit = kind === 'send'
        ? () => router.submitSend('target', parts)
        : kind === 'request'
        ? () => router.request('target', parts, 1000, () => {}, () => {}, undefined, undefined,
          { id: 1n, deadlineMs: performance.now() + 1000 })
        : () => router.replyParts(zlink.RoutingId.from('target'), {}, parts);
      if (fail) {
        assert.throws(submit, error => error === failure);
        assert.equal(close.mock.calls.filter(c => c.this === native).length, 1);
      } else {
        submit();
        assert.ok(seen[0] instanceof zlink.Message);
        assert.strictEqual(seen[0], native);
        assert.strictEqual(seen[0].data(), data);
        assert.equal(close.mock.callCount(), 0, 'successful submit leaves ownership with binding');
        // This test binding only observes transfer; release its captured storage here.
        native.close();
      }
    });
  }
}

test('native header writer releases storage when a dynamic field write fails', t => {
  const codecs = { serializers: new Map() };
  envelope.closeMessages(request(codecs));
  const allocate = t.mock.method(zlink.Message, 'allocate');
  const close = t.mock.method(zlink.Message.prototype, 'close');
  const failure = new Error('dynamic field write failed');
  t.mock.method(Buffer.prototype, 'write', () => { throw failure; });
  assert.throws(() => request(codecs), error => error === failure);
  assert.equal(allocate.mock.callCount(), 1);
  assert.equal(close.mock.callCount(), 1);
  assert.strictEqual(close.mock.calls[0].this, allocate.mock.calls[0].result);
});
''')
