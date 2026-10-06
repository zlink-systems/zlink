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
import { fromProto } from '@zlink-systems/framework-codec-protobuf';
import messages from './generated/messages.cjs';
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
      assert.equal(header.name, 'Ping');
      sentNames.push(header.name);
      const payload = Ping.decode(frame.payload);
      const isRequest = header.kind === ZlinkStreamMessageKind.Request;
      assert.equal(payload.text, isRequest ? 'rank' : 'hello');
      socket.send(
        encodeStreamWireFrame(
          encodeStreamWireHeader({
            ...header,
            name: isRequest ? 'Pong' : 'Ping',
            kind: isRequest ? ZlinkStreamMessageKind.Response : ZlinkStreamMessageKind.Send
          }),
          isRequest ? Pong.encode(new Pong({ rank: 7 })).finish() : frame.payload
        )
      );
    });
  });
  try {
    const result = await runProtobuf(`ws://127.0.0.1:${server.address().port}`);
    assert.equal(result, 'protobuf: push=hello, reply.rank=7');
    assert.deepEqual(sentNames, ['Ping', 'Ping']);
    console.log(result);
  } finally {
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
