'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/contracts/Errors/ZLinkFrameworkException');
const golden = require('../../../../runtime/conformance/admission-queue-messages-v1.json');
const packets = require('../../packages/framework/dist/runtime/streams/admission-queue-messages');
const {
  readZLinkDecoratorMetadata
} = require('../../packages/framework/dist/contracts/Handlers/Attributes');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');

const codecs = {
  serializers: new Map([
    [
      'application/x-custom',
      {
        serialize() {
          assert.fail('admission queue packets must use the default JSON serializer');
        }
      }
    ]
  ])
};

const contracts = {
  enqueue: [packets.AdmissionEnqueue, packets.readEnqueue, packets.readEnqueueReply],
  report: [packets.AdmissionNodeReport, packets.readReport, packets.readReportReply],
  'admit-permit': [
    packets.AdmissionAdmitPermit,
    packets.readAdmitPermit,
    packets.readAdmitPermitReply
  ],
  'release-permit': [packets.AdmissionReleasePermit, packets.readReleasePermit]
};

function contract(packetName) {
  return contracts[packetName.split('.').at(-1)];
}

function readParts(parts, inspect) {
  const messages = parts.map((part) => ZLinkBufferMessage.from(part));
  try {
    return inspect(messages);
  } finally {
    for (const message of messages) message.close();
    envelope.closeMessages(parts);
  }
}

function decode(payload, packetName, read) {
  return read(
    envelope.decodeChannelPayload(
      {
        header: { messageName: packetName, contentType: envelope.JSON_CONTENT_TYPE },
        payload: Buffer.from(JSON.stringify(payload))
      },
      codecs
    )
  );
}

for (const {
  name,
  packetName,
  replyPacketName,
  kind,
  request,
  response,
  send
} of golden.messages) {
  test(`admission queue wire: ${name}`, () => {
    const [Packet, read, readReply] = contract(packetName);
    assert.equal(readZLinkDecoratorMetadata(Packet)[0].packetName, packetName);
    const payload = kind === 'request' ? request : send;
    const wireKind = kind === 'request' ? 1 : 3;
    const parts = envelope.encodeChannelEnvelopeParts(
      wireKind,
      'login',
      undefined,
      new Packet(payload),
      undefined,
      undefined,
      codecs
    );
    const header = readParts(parts, (messages) => {
      const frame = envelope.decodeChannelEnvelope(messages);
      assert.equal(frame.header.kind, wireKind);
      assert.equal(frame.header.messageName, packetName);
      assert.equal(frame.header.contentType, envelope.JSON_CONTENT_TYPE);
      assert.deepEqual(JSON.parse(frame.payload.toString()), payload);
      assert.deepEqual(read(envelope.decodeChannelPayload(frame, codecs)), payload);
      return frame.header;
    });
    if (response !== undefined) {
      const reply = envelope.encodeChannelReplyParts(header, response, codecs);
      readParts(reply, (messages) => {
        const frame = envelope.decodeChannelEnvelope(messages);
        assert.equal(frame.header.kind, 2);
        assert.equal(frame.header.messageName, replyPacketName);
        assert.equal(frame.header.contentType, envelope.JSON_CONTENT_TYPE);
        assert.deepEqual(JSON.parse(frame.payload.toString()), response);
        assert.deepEqual(
          readReply(envelope.decodeChannelReply(messages, codecs, packetName)),
          response
        );
      });
    } else {
      assert.equal(replyPacketName, undefined);
    }
  });
}

const enqueue = golden.messages.find(({ name }) => name === 'enqueue-position');
for (const wrongName of [
  enqueue.packetName,
  golden.messages.find(({ name }) => name === 'report').replyPacketName,
  'UnexpectedReply'
]) {
  test(`admission queue reply rejects packet name ${wrongName}`, () => {
    const parts = envelope.encodeChannelEnvelopeParts(
      2,
      'login',
      wrongName,
      enqueue.response,
      undefined,
      undefined,
      undefined,
      '0123456789abcdef0123456789abcdef'
    );
    readParts(parts, (messages) => {
      assert.throws(() => envelope.decodeChannelReply(messages, codecs, enqueue.packetName), {
        name: 'ZLinkFrameworkException',
        kind: ZLinkFrameworkErrorKind.ProtocolError
      });
    });
  });
}

for (const { name, packetName, payload, responsePayload, expect } of golden.invalid) {
  test(`admission queue wire: ${name}`, () => {
    const [, read, readReply] = contract(packetName);
    if (expect === 'accepted') {
      const { future, ...known } = payload;
      assert.deepEqual(decode(payload, packetName, read), known);
    } else {
      assert.throws(
        () => decode(payload ?? responsePayload, packetName, payload ? read : readReply),
        {
          name: 'ZLinkFrameworkException',
          kind: ZLinkFrameworkErrorKind.ProtocolError
        }
      );
    }
  });
}
