'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { fixture } = require('./fixtures/raw-mesh-close');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const { SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME, SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE } = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

function enqueue(f, parts, options = {}) {
  f.runtime.mailbox.tryEnqueue({ owner: 'local', domain: 'application',
    parts: [wire.encodeNodeSendHeader(), wire.encodeMultipartApplicationPayload(parts, SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME, SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE)], ...options });
}
function receive(f, capacity = 4) {
  const ready = f.backend.createReadyBatch(1);
  f.backend.drainReady(ReadyDomain.Application, ready);
  const claim = ready.takeClaim(0);
  try { return claim.recvBatch(f.backend.createReceiveBatch(1, capacity)); }
  finally { claim.release(); ready.close(); }
}

test('ordinary claim materializes only final part views over its retained frame', async t => {
  const f = fixture();
  try {
    enqueue(f, [Buffer.from('header'), Buffer.from('body')]);
    const subarray = t.mock.method(Buffer.prototype, 'subarray');
    const result = receive(f);
    const views = subarray.mock.callCount();
    assert.equal(result.records.length, 1);
    assert.deepEqual(result.records[0].parts.map(p => p.data().toString()), ['header', 'body']);
    assert.equal(views, 2, 'one view per final part, no intermediate multipart payload view');
    for (const p of result.records[0].parts) p.close();
  } finally { await f.backend.close(); }
});

test('claim capacity returns the original record for later receive without closing its lease', async () => {
  const f = fixture();
  let closed = 0;
  try {
    enqueue(f, [Buffer.from('header'), Buffer.from('body')], { applicationJob: { close() { closed++; } } });
    assert.equal(receive(f, 1).ok, false);
    assert.equal(closed, 0);
    const accepted = receive(f, 2);
    assert.equal(accepted.records[0].parts[1].data().toString(), 'body');
    accepted.records[0].releaseRetainedIngress();
    assert.equal(closed, 1);
  } finally { await f.backend.close(); }
});

test('malformed multipart releases its lease and leaves the following record receivable', async () => {
  const f = fixture();
  let closed = 0;
  try {
    const frame = wire.encodeMultipartApplicationPayload([Buffer.from('valid')], SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME, SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE);
    frame.writeUInt32BE(0xffffffff, frame.length - 9);
    f.runtime.mailbox.tryEnqueue({ owner: 'local', domain: 'application', parts: [wire.encodeNodeSendHeader(), frame], applicationJob: { close() { closed++; } } });
    enqueue(f, [Buffer.from('next')]);
    const result = receive(f);
    assert.equal(closed, 1);
    assert.equal(result.records.length, 1);
    assert.equal(result.records[0].parts[0].data().toString(), 'next');
  } finally { await f.backend.close(); }
});

test('direct multipart decoding rejects every truncated frame and closes partial materialization', () => {
  const frame = wire.encodeMultipartApplicationPayload([Buffer.from('one'), Buffer.from('two')], SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME, SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE);
  for (let end = 0; end < frame.length; end++) {
    assert.throws(() => wire.decodeMultipartApplicationParts(frame.subarray(0, end), part => part), wire.ServiceWireProtocolError);
  }
  const mutations = [
    bytes => { bytes[0] = 0; },
    bytes => { bytes[5] = 0; },
    bytes => { bytes[6] ^= 1; },
    bytes => { bytes.writeUInt32BE(0, bytes.length - 18); },
    bytes => { bytes.writeUInt32BE(0xffffffff, bytes.length - 18); },
    bytes => { bytes.writeUInt32BE(0xffffffff, bytes.length - 14); }
  ];
  for (const mutate of mutations) {
    const bad = Buffer.from(frame); mutate(bad);
    assert.throws(() => wire.decodeMultipartApplicationParts(bad, part => part), wire.ServiceWireProtocolError);
  }
  const failure = new Error('materializer failed');
  const released = [];
  let created = 0;
  assert.throws(() => wire.decodeMultipartApplicationParts(frame, part => {
    if (created++ === 1) throw failure;
    return part;
  }, part => released.push(part.toString())), error => error === failure);
  assert.deepEqual(released, ['one']);
  const backing = new Uint8Array(frame.length + 2);
  backing.set(frame, 1);
  const input = backing.subarray(1, frame.length + 1);
  const parts = wire.decodeMultipartApplicationParts(input, part => part);
  assert.deepEqual(parts.map(part => part.toString()), ['one', 'two']);
  assert.strictEqual(parts[0].buffer, backing.buffer);
});
