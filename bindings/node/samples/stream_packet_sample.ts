// SPDX-License-Identifier: MPL-2.0

'use strict';

const assert = require('node:assert/strict');
const { once } = require('node:events');
const net = require('node:net');
const zlink = require('@zlink-systems/zlink');
const { frame, reservePort } = require('./sample_support');

async function main() {
// --8<-- [start:doc]
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const ctx = zlink.createContext();
  const stream = zlink.createStreamSocket(ctx);
  const packet = new zlink.StreamPacket();
  let client;

  try {
    stream.options.recvMode = zlink.StreamRecvMode.Packet;
    stream.bind(endpoint);
    client = net.createConnection({ host: '127.0.0.1', port });
    await once(client, 'connect');
    client.write(frame(Buffer.from('hello-stream')));

    for (let attempt = 0; attempt < 200; attempt += 1) {
      if (stream.recvPacket(packet, zlink.RecvFlags.DontWait)) break;
      await new Promise((resolve) => setTimeout(resolve, 2));
    }
    assert.ok(packet.routingId instanceof zlink.RoutingId);
    assert.equal(packet.header.data().length, 0);
    assert.equal(packet.body.data().toString(), 'hello-stream');
    console.log('[stream/packet] send: "hello-stream" -> recv: "hello-stream"');
  } finally {
    packet.close();
    if (client) client.destroy();
    stream.close();
    ctx.close();
  }
// --8<-- [end:doc]
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
