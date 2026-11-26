'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const metadataCodec = require('../../packages/framework/dist/runtime/foundation/service-metadata-codec');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const { encodeChannelHeader } = require('./helpers/protobuf-channel-header');
const {
  zlink
} = require('../../packages/framework/dist/runtime/channels/generated/channel-header-v1');

function frame(entries) {
  const parts = [Buffer.from([1, entries.length])];
  for (const { key, value } of entries) {
    const k = Buffer.from(key);
    const v = Buffer.from(value);
    const length = Buffer.alloc(2);
    length.writeUInt16BE(v.length);
    parts.push(Buffer.from([k.length]), k, length, v);
  }
  return Buffer.concat(parts);
}

for (const [name, entries, accepted, expectedSize] of [
  [
    'UTF-8 content within limit but field 13 exceeds it',
    [
      { key: 'a', value: 'x'.repeat(500) },
      { key: 'b', value: 'x'.repeat(510) }
    ],
    false,
    1028
  ],
  ['field 13 is exactly 1024 bytes', [{ key: 'k', value: 'v'.repeat(1015) }], true, 1024],
  ['field 13 is 1025 bytes', [{ key: 'k', value: 'v'.repeat(1016) }], false, 1025]
]) {
  test(`application metadata boundary: ${name}`, async (t) => {
    assert.equal(
      zlink.framework.channel.v1.ChannelHeader.encode({ metadata: entries }).finish().length,
      expectedSize
    );
    assert.ok(frame(entries).length <= 1024);
    const metadata = new Map(entries.map(({ key, value }) => [key, value]));
    const operations = [
      () => {
        const parts = envelope.encodeChannelEnvelopeParts(
          3,
          'api',
          'Notice',
          {},
          undefined,
          undefined,
          undefined,
          undefined,
          false,
          metadata
        );
        envelope.closeMessages(parts);
      },
      () =>
        envelope.decodeChannelHeader([
          new ZLinkBufferMessage(
            encodeChannelHeader({
              kind: 3,
              channelName: 'api',
              messageName: 'Notice',
              contentType: 'application/json',
              metadata: entries
            })
          )
        ]),
      () => metadataCodec.encodeServiceMetadataFrame(metadata),
      () => metadataCodec.validateServiceMetadataFrame(frame(entries))
    ];
    for (const [index, operation] of operations.entries()) {
      await t.test(
        ['envelope encode', 'envelope decode', 'frame encode', 'frame decode'][index],
        () => {
          if (accepted) assert.doesNotThrow(operation);
          else assert.throws(operation, /1024/);
        }
      );
    }
  });
}
