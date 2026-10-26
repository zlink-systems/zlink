const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { ZLinkSubmitStatus } = require('../../packages/framework/dist/runtime/messaging/submission-result');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

for (const classification of [true, undefined]) {
  test(`node send skips peer snapshots when manual classification is ${classification}`, async () => {
    let sent = 0;
    const node = {
      routingId: 'source',
      status() { throw new Error('Send must use the owned local RID.'); },
      peers() { throw new Error('Send must not materialize a monitoring snapshot.'); },
      async sendToNode() { sent++; return SubmitResult.Ok; }
    };
    const transport = new framework.ZLinkRuntimeRouteTransport(
      () => undefined, undefined, () => ({ meshNode: () => node }),
      undefined, () => classification, undefined, undefined, () => false
    );
    const result = await transport.submit('mesh', 'target', 'Probe', { value: 1 });
    assert.equal(result.status, ZLinkSubmitStatus.Submitted);
    assert.equal(sent, 1);
  });
}

test('manual target exclusion preserves admitted-peer and absent-target results', async () => {
  for (const known of [true, false]) {
    let sent = 0;
    const node = {
      status: () => ({ routingId: 'source' }),
      peers: () => known ? [{ routingId: 'target' }] : [],
      async sendToNode() { sent++; return SubmitResult.Ok; }
    };
    const transport = new framework.ZLinkRuntimeRouteTransport(
      () => undefined, undefined, () => ({ meshNode: () => node }),
      undefined, () => false, undefined, undefined, () => false
    );
    const result = await transport.submit('mesh', 'target', 'Probe', { value: 1 });
    assert.equal(result.status, known
      ? ZLinkSubmitStatus.Submitted : ZLinkSubmitStatus.TargetNotFound);
    assert.equal(sent, known ? 1 : 0);
  }
});

test('local node send reuses the owned RID without building status snapshots', async () => {
  const node = {
    routingId: 'source',
    status() { throw new Error('Local send must not build a status snapshot.'); },
    peers() { throw new Error('Local send must not build a peer snapshot.'); }
  };
  let localSource;
  const transport = new framework.ZLinkRuntimeRouteTransport(
    () => undefined, undefined, () => ({ meshNode: () => node }), undefined, undefined,
    (mesh, source) => { localSource = source; return { status: ZLinkSubmitStatus.Submitted }; },
    undefined, () => false
  );
  assert.equal((await transport.submit('mesh', 'source', 'Probe', { value: 1 })).status,
    ZLinkSubmitStatus.Submitted);
  assert.equal(localSource, 'source');
});

test('service multipart writes borrowed slices once into the owned wire frame', () => {
  const storage = Uint8Array.from([99, 1, 2, 3, 88]);
  const parts = [storage.subarray(1, 4), Buffer.from([4, 5])];
  const originalFrom = Buffer.from;
  let intermediateBuffers = 0;
  let frame;
  Buffer.from = function(value, ...args) {
    if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer) intermediateBuffers++;
    return originalFrom(value, ...args);
  };
  try {
    frame = wire.encodeMultipartApplicationPayload(parts, 'Probe', 'application/octet-stream');
  } finally {
    Buffer.from = originalFrom;
  }
  assert.equal(intermediateBuffers, 0, 'No intermediate byte copies or Buffer views.');
  const decoded = wire.decodeApplicationPayloadView(frame);
  assert.equal(decoded.packetName, 'Probe');
  assert.equal(decoded.contentType, 'application/octet-stream');
  assert.deepEqual([...decoded.payload], [0, 0, 0, 2, 0, 0, 0, 3, 1, 2, 3, 0, 0, 0, 2, 4, 5]);
  storage.fill(0);
  parts[1].fill(0);
  assert.deepEqual([...decoded.payload.subarray(8, 11)], [1, 2, 3]);
  assert.throws(() => wire.encodeMultipartApplicationPayload([], 'Probe', 'application/octet-stream'));
  assert.throws(() => wire.decodeApplicationPayloadView(frame.subarray(0, frame.length - 1)));
});

// Spec 08 E1 allows the one write into the outbound frame. Every frame byte
// is written by the encoder, so a small frame takes pooled storage: no zero
// fill and no backing store per message. Unallocated pool bytes are dirtied
// first, so a byte the encoder skipped would differ from the expected layout.
test('small service frames take pooled storage and write every byte', () => {
  const text8 = value => Buffer.concat([Buffer.from([Buffer.byteLength(value)]), Buffer.from(value)]);
  const u32 = value => { const bytes = Buffer.alloc(4); bytes.writeUInt32BE(value); return bytes; };
  const expected = (packetName, contentType, payload) => {
    const body = Buffer.concat([text8(packetName), text8(contentType), u32(payload.length), payload]);
    return Buffer.concat([Buffer.from([1]), u32(body.length), body]);
  };
  // Start where the pool has room for both frames.
  let probe = Buffer.allocUnsafe(1);
  while (probe.buffer.byteLength - probe.byteOffset < 1024) probe = Buffer.allocUnsafe(1);
  new Uint8Array(probe.buffer, probe.byteOffset + 1).fill(0xa5);
  const single = wire.encodeApplicationPayload({
    packetName: 'Probe', contentType: 'application/json', payload: Buffer.from('{"body":1}')
  });
  const multipart = wire.encodeMultipartApplicationPayload(
    [Buffer.from('header'), Buffer.from('body')], 'ZLinkFrameworkMultipart', 'application/x-zlink-multipart'
  );
  assert.deepEqual(single, expected('Probe', 'application/json', Buffer.from('{"body":1}')));
  assert.deepEqual(multipart, expected('ZLinkFrameworkMultipart', 'application/x-zlink-multipart',
    Buffer.concat([u32(2), u32(6), Buffer.from('header'), u32(4), Buffer.from('body')])));
  assert.equal(single.buffer, probe.buffer, 'A small single frame owns no separate backing store.');
  assert.equal(multipart.buffer, probe.buffer, 'A small multipart frame owns no separate backing store.');
});

test('service payload encoders retain canonical text and frame ownership at text boundaries', () => {
  for (const packetName of ['Probe', '가'.repeat(85), 'a'.repeat(255)]) {
    const payload = Uint8Array.from([1, 2, 3]);
    const frame = wire.encodeApplicationPayload({ packetName, contentType: 'application/json', payload });
    payload.fill(9);
    const decoded = wire.decodeApplicationPayloadView(frame);
    assert.equal(decoded.packetName, packetName);
    assert.equal(decoded.contentType, 'application/json');
    assert.deepEqual([...decoded.payload], [1, 2, 3]);
  }
  for (const packetName of ['', 'a'.repeat(256), '가'.repeat(86), '\0', '\ud800']) {
    assert.throws(() => wire.encodeApplicationPayload({
      packetName, contentType: 'application/json', payload: Buffer.alloc(0)
    }), wire.ServiceWireProtocolError);
    assert.throws(() => wire.encodeMultipartApplicationPayload(
      [Buffer.alloc(0)], packetName, 'application/json'
    ), wire.ServiceWireProtocolError);
  }
});
