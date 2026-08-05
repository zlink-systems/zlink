// SPDX-License-Identifier: MPL-2.0

'use strict';

const assert = require('node:assert/strict');
const { once } = require('node:events');
const net = require('node:net');
const zlink = require('@zlink-systems/zlink');

async function reservePort() {
  const srv = net.createServer();
  srv.listen(0, '127.0.0.1');
  await once(srv, 'listening');
  const { port } = srv.address();
  await new Promise((resolve, reject) => srv.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function main() {
// --8<-- [start:doc]
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const ctx = zlink.createContext();
  const server = zlink.createPairSocket(ctx);
  const client = zlink.createPairSocket(ctx);

  try {
    const serverMonitor = server.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const clientMonitor = client.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    try {
      server.bind(endpoint);
      client.connect(endpoint);
      serverMonitor.recv();
      clientMonitor.recv();
    } finally {
      serverMonitor.close();
      clientMonitor.close();
    }

    const sent = 'hello-pair';
    client.send().message(Buffer.from(sent)).submit();

    const received = new zlink.Received();
    server.recv(received);
    try {
      const recv = received.parts[0].data().toString();
      assert.equal(recv, sent);
      console.log(`[pair/recv] send: "${sent}" → recv: "${recv}"`);
    } finally {
      received.close();
    }
  } finally {
    client.close();
    server.close();
    ctx.close();
  }
// --8<-- [end:doc]
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
