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
import { runReceiving } from './dist/StreamClient/receiving.js';

test('receiving tutorial pumps, unsubscribes, waits and counts actual WebSocket packets', async () => {
  const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await once(server, 'listening');
  server.on('connection', (socket) => {
    const send = (name, payload) =>
      socket.send(
        encodeStreamWireFrame(
          encodeStreamWireHeader({
            kind: ZlinkStreamMessageKind.Send,
            codec: ZlinkStreamCodec.Json,
            flags: 0,
            name,
            metadata: {}
          }),
          new TextEncoder().encode(JSON.stringify(payload))
        )
      );
    socket.on('message', (bytes) => {
      const frame = decodeStreamWireFrame(new Uint8Array(bytes));
      const header = decodeStreamWireHeader(frame.header);
      if (header.kind === ZlinkStreamMessageKind.Control) return;
      assert.equal(header.name, 'ReceivingStage');
      const { stage } = JSON.parse(new TextDecoder().decode(frame.payload));
      switch (stage) {
        case 'pump':
        case 'unsubscribed':
          send('LeaderboardUpdate', { rank: 7 });
          send('Ready', { stage });
          break;
        case 'match':
          send('MatchFound', { matchId: 'other' });
          send('MatchFound', { matchId: 'match-7f3a' });
          break;
        case 'orders':
          send('OrderChanged', { status: 'paid' });
          send('OrderChanged', { status: 'shipped' });
          break;
        default:
          assert.fail('Unexpected receiving stage');
      }
    });
  });
  try {
    const result = await runReceiving(`ws://127.0.0.1:${server.address().port}`);
    assert.equal(
      result,
      'receiving: handler=1, frames=1, match=match-7f3a, sequence=paid,shipped, count=2'
    );
    console.log(result);
  } finally {
    for (const socket of server.clients) socket.terminate();
    server.close();
    await once(server, 'close');
  }
});
