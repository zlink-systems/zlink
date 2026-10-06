const assert = require('node:assert/strict');
const test = require('node:test');
const wire = require('@zlink-systems/stream-wire');
const connector = require('../../packages/stream-connector/dist');
const { ZlinkStreamHeaderCodec } = require('../../packages/stream-connector/dist/Runtime/Protocol/ZlinkStreamHeaderCodec');
const { decodeStreamHeader } = require('../../packages/framework/dist/runtime/streams/protocol');

for (const [label, offset, value] of [
  ['unknown kind', 1, 255],
  ['unknown codec', 2, 255],
  ['Control JSON codec', 2, wire.ZlinkStreamCodec.Json],
  ['Control compression flag', 3, wire.ZlinkStreamHeaderFlags.PayloadCompressed],
  ['Control request sequence flag', 3, wire.ZlinkStreamHeaderFlags.HasRequestSeq],
  ['Control metadata flag', 3, wire.ZlinkStreamHeaderFlags.HasMetadata],
  ['Control correlation flag', 3, wire.ZlinkStreamHeaderFlags.HasCorrelationId],
  ['Control flow flag', 3, wire.ZlinkStreamHeaderFlags.HasFlowId],
  ['Control Actor slot flag', 3, wire.ZlinkStreamHeaderFlags.HasActorSlot]
]) {
  test(`server, connector and wire reject ${label}`, () => {
    const header = wire.encodeStreamWireHeader({
      kind: wire.ZlinkStreamMessageKind.Control,
      codec: wire.ZlinkStreamCodec.Raw,
      flags: wire.ZlinkStreamHeaderFlags.None,
      name: '$zlink.heartbeat.ping',
      metadata: new Map()
    });
    assert.equal(decodeStreamHeader(header).kind, wire.ZlinkStreamMessageKind.Control);
    assert.equal(ZlinkStreamHeaderCodec.decode(header).kind, wire.ZlinkStreamMessageKind.Control);
    header[offset] = value;
    assert.throws(() => wire.decodeStreamWireHeader(header), /Unknown stream|Control packet/);
    assert.throws(() => decodeStreamHeader(header), /Unknown stream|Control packet/);
    assert.throws(() => ZlinkStreamHeaderCodec.decode(header),
      error => error.error?.code === connector.ZlinkStreamErrorCode.FrameDecodeFailed);
  });
}

test('wire encoder validates Control fields even when the caller omits their flags', () => {
  const header = {
    kind: wire.ZlinkStreamMessageKind.Control,
    codec: wire.ZlinkStreamCodec.Raw,
    flags: wire.ZlinkStreamHeaderFlags.None,
    name: '$zlink.heartbeat.ping',
    metadata: new Map()
  };
  for (const fields of [
    { requestSeq: 1n },
    { metadata: new Map([['key', 'value']]) },
    { correlationId: 'correlation' },
    { flowId: '01234567-89ab-7def-8123-456789abcdef', flowOrigin: 1 },
    { actorSlot: 1 }
  ]) {
    assert.throws(() => wire.encodeStreamWireHeader({ ...header, ...fields }), /Control packet/);
  }
});
