import assert from 'node:assert/strict';
import { once } from 'node:events';
import test from 'node:test';
import { WebSocketServer } from 'ws';
import {
  decodeStreamWireFrame,
  decodeStreamWireHeader,
  encodeStreamWireFrame,
  encodeStreamWireHeader,
  ZlinkStreamCodec,
  ZlinkStreamMessageKind
} from '@zlink-systems/stream-wire';
import {
  createZlinkStreamProtobufCodec,
  createZlinkStreamProtobufEnvelopeCodec,
  fromProto
} from '@zlink-systems/framework-codec-protobuf';
import messages from './generated/messages.cjs';
import {
  zlinkStreamConnectorFactory,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import { runProtobuf } from './dist/StreamClient/protobuf.js';

const { Ping, Pong } = messages.tutorial;

test('generated Protobuf push and explicitly typed reply over WebSocket', async () => {
  // This peer belongs to the test harness. Application snippets use only the connector.
  const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await once(server, 'listening');
  const sentNames = [];
  server.on('connection', (socket) => {
    socket.on('message', (bytes) => {
      const frame = decodeStreamWireFrame(new Uint8Array(bytes));
      const header = decodeStreamWireHeader(frame.header);
      if (header.kind === ZlinkStreamMessageKind.Control) return;
      assert.equal(header.codec, ZlinkStreamCodec.Protobuf);
      assert.ok(header.name === 'Ping' || header.name === 'Pong');
      sentNames.push(header.name);
      const Type = header.name === 'Pong' ? Pong : Ping;
      const payload = Type.decode(frame.payload);
      const isRequest = header.kind === ZlinkStreamMessageKind.Request;
      if (Type === Ping) assert.equal(payload.text, isRequest ? 'rank' : 'hello');
      else assert.equal(payload.rank, 3);
      socket.send(
        encodeStreamWireFrame(
          encodeStreamWireHeader({
            ...header,
            name: isRequest ? 'Pong' : header.name,
            kind: isRequest ? ZlinkStreamMessageKind.Response : ZlinkStreamMessageKind.Send
          }),
          isRequest ? Pong.encode(new Pong({ rank: 7 })).finish() : frame.payload
        )
      );
      if (!isRequest && Type === Ping) {
        socket.send(
          encodeStreamWireFrame(
            encodeStreamWireHeader({ ...header, name: 'Pong', kind: ZlinkStreamMessageKind.Send }),
            Pong.encode(new Pong({ rank: 3 })).finish()
          )
        );
      }
    });
  });
  try {
    const result = await runProtobuf(`ws://127.0.0.1:${server.address().port}`);
    assert.equal(result, 'protobuf: Ping=hello, Pong.rank=3, reply.rank=7');
    assert.deepEqual(sentNames, ['Ping', 'Pong', 'Ping', 'Ping']);
    console.log(result);
  } finally {
    for (const client of server.clients) client.terminate();
    server.close();
    await once(server, 'close');
  }
});

test('fromProto rejects the wrong codec and malformed bytes', () => {
  assert.throws(
    () => fromProto({ codec: ZlinkStreamCodec.Json, payload: new Uint8Array() }, Pong),
    /not Protobuf/
  );
  assert.throws(() =>
    fromProto({ codec: ZlinkStreamCodec.Protobuf, payload: new Uint8Array([0x08, 0x80]) }, Pong)
  );
});

test('one codec honors generated decoding types and encoding constructors', () => {
  const codec = createZlinkStreamProtobufCodec(Ping);
  for (const [Type, value] of [
    [Ping, new Ping({ text: 'hello' })],
    [Pong, new Pong({ rank: 3 })]
  ]) {
    const received = codec.decode(
      { codec: ZlinkStreamCodec.Protobuf, payload: Type.encode(value).finish() },
      Type
    );
    assert.ok(received instanceof Type);
    assert.deepEqual(received, value);
    const encoded = codec.encode(value);
    assert.deepEqual(encoded.payload, Type.encode(value).finish());
    const decoded = codec.decode(encoded, Type);
    assert.ok(decoded instanceof Type);
    assert.deepEqual(decoded, value);
  }
  assert.deepEqual(codec.encode({ rank: 3 }, Pong).payload, Pong.encode({ rank: 3 }).finish());
  class LegacyMessage {
    text = 'legacy';
  }
  const legacy = codec.encode(new LegacyMessage());
  assert.equal(legacy.messageType, LegacyMessage);
  assert.equal(codec.decode(legacy).text, 'legacy');
  const fallback = codec.decode(codec.encode({ text: 'fallback' }));
  assert.ok(fallback instanceof Ping);
  assert.equal(fallback.text, 'fallback');
});

test('envelope codec forwards the optional runtime decoding type', () => {
  const codec = createZlinkStreamProtobufEnvelopeCodec({
    encode: (value) => ({ codec: ZlinkStreamCodec.Protobuf, payload: Pong.encode(value).finish() }),
    decode: (payload, Type = Pong) => fromProto(payload, Type)
  });
  const encoded = codec.encode(new Pong({ rank: 3 }));
  assert.ok(codec.decode(encoded, Pong) instanceof Pong);
  assert.equal(codec.decode(encoded, Pong).rank, 3);
  assert.ok(codec.decode(encoded) instanceof Pong);
  const ping = {
    codec: ZlinkStreamCodec.Protobuf,
    payload: Ping.encode(new Ping({ text: 'hello' })).finish()
  };
  assert.ok(codec.decode(ping, Ping) instanceof Ping);
  assert.equal(codec.decode(ping, Ping).text, 'hello');
});

test('submit accepts a generated Protobuf reply constructor', async () => {
  const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await once(server, 'listening');
  server.on('connection', (socket) =>
    socket.on('message', (bytes) => {
      const frame = decodeStreamWireFrame(new Uint8Array(bytes));
      const header = decodeStreamWireHeader(frame.header);
      if (header.kind === ZlinkStreamMessageKind.Control) return;
      socket.send(
        encodeStreamWireFrame(
          encodeStreamWireHeader({
            ...header,
            name: 'Pong',
            kind: ZlinkStreamMessageKind.Response
          }),
          Pong.encode(new Pong({ rank: 7 })).finish()
        )
      );
    })
  );
  const connector = zlinkStreamConnectorFactory.create({
    endpoint: `ws://127.0.0.1:${server.address().port}`,
    codec: createZlinkStreamProtobufCodec(Ping),
    dispatchMode: ZlinkStreamDispatchMode.Immediate
  });
  try {
    await connector.connect();
    const pending = connector
      .request(new Ping({ text: 'rank' }))
      .submit(Pong, new AbortController().signal);
    assert.ok(pending instanceof Promise);
    const reply = await pending;
    assert.ok(reply instanceof Pong);
    assert.equal(reply.rank, 7);
  } finally {
    await connector.close();
    for (const socket of server.clients) socket.terminate();
    server.close();
    await once(server, 'close');
  }
});
