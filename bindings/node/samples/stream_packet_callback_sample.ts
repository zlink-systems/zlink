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
  let client;

  try {
    stream.bind(endpoint);
    client = net.createConnection({ host: '127.0.0.1', port });
    await once(client, 'connect');

    const received = await new Promise<{
      sourceRid: InstanceType<typeof zlink.RoutingId>;
      header: InstanceType<typeof zlink.Message>;
      body: InstanceType<typeof zlink.Message>;
    }>((resolve, reject) => {
      try {
        stream.setPacketHandler((sourceRid, header, body) => {
          resolve({ sourceRid, header, body });
        });
      } catch (error) {
        reject(error);
        return;
      }
      const payload = Buffer.from('hello-stream');
      client.write(frame(payload));
    });

    try {
      assert.ok(received.sourceRid instanceof zlink.RoutingId);
      assert.equal(received.header.data().length, 0);
      assert.equal(received.body.data().toString(), 'hello-stream');
      console.log('[stream/packet-callback] send: "hello-stream" -> recv: "hello-stream"');
    } finally {
      received.header.close();
      received.body.close();
    }
  } finally {
    if (client) {
      client.destroy();
    }
    stream.close();
    ctx.close();
  }
// --8<-- [end:doc]
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
