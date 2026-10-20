const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkSpotActorPacketDrain
} = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-drain');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/contracts');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  createMessagePackSerializer
} = require('../../packages/framework-codec-msgpack/dist/server/framework.cjs');

for (const binarySerializer of [false, true]) {
  for (const frameworkError of [false, true]) {
    test(`Actor Error profile uses JSON code/message (binary=${binarySerializer}, framework=${frameworkError})`, async () => {
      const error = frameworkError
        ? new ZLinkFrameworkException(ZLinkFrameworkErrorKind.NotFound, 'handler missing')
        : new Error('application failed');
      const replies = [];
      const drain = new ZLinkSpotActorPacketDrain({
        messageSerializers: binarySerializer
          ? new Map([['application/x-msgpack', createMessagePackSerializer()]])
          : undefined,
        actorPacketHandler: async () => {
          throw error;
        },
        waitIdle: async () => undefined,
        replyActorNoBind: (info, parts) => {
          assert.equal(info.requestId, 42n);
          assert.equal(parts.length, 1);
          replies.push(Buffer.from(parts[0].data()));
          for (const part of parts) part.close();
        }
      });
      const received = [
        protocol.encodeStreamHeader({
          kind: protocol.ZLinkStreamMessageKind.Request,
          codec: protocol.ZLinkStreamCodec.Json,
          flags: protocol.ZLinkStreamHeaderFlags.HasRequestSeq,
          requestSeq: 7n,
          name: 'ActorReq',
          metadata: new Map()
        }),
        Buffer.from('{}')
      ].map((bytes, index) => ({
        info: {
          actor: { nodeRid: 'actor-node', actorId: 'actor', generation: 1n },
          sourceNodeRid: 'requester-node',
          sourceSessionRid: 'session',
          requestId: 42n,
          flags: 1
        },
        message: new ZLinkBufferMessage(bytes),
        more: index === 0
      }));
      try {
        await drain.drain({ recvActorPart: () => received.shift() ?? null });
        assert.equal(replies.length, 1);
        const frame = protocol.decodeStreamFrame(replies[0]);
        const header = protocol.decodeStreamHeader(frame.header);
        assert.equal(header.kind, protocol.ZLinkStreamMessageKind.Error);
        assert.equal(header.codec, protocol.ZLinkStreamCodec.Json);
        assert.equal(header.name, '');
        assert.equal(header.requestSeq, 7n);
        assert.deepEqual(JSON.parse(Buffer.from(frame.payload).toString()), {
          code: frameworkError ? 'not_found' : 'internal_failure',
          message: error.message
        });
      } finally {
        drain.dispose();
        for (const part of received) part.message.close();
      }
    });
  }
}
