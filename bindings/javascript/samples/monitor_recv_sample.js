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
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const ctx = zlink.createContext();
  const server = zlink.createPairSocket(ctx);
  const client = zlink.createPairSocket(ctx);
  const serverMonitor = server.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
  const clientMonitor = client.monitorOpen([zlink.MonitorEventType.ConnectionReady]);

  try {
    server.bind(endpoint);
    client.connect(endpoint);

    const serverEvent = serverMonitor.recv();
    const clientEvent = clientMonitor.recv();
    assert.equal(serverEvent.event, zlink.MonitorEventType.ConnectionReady);
    assert.equal(clientEvent.event, zlink.MonitorEventType.ConnectionReady);
    console.log('[monitor/recv] recv: "connection-ready"');
  } finally {
    clientMonitor.close();
    serverMonitor.close();
    client.close();
    server.close();
    ctx.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
