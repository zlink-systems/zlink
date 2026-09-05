// SPDX-License-Identifier: MPL-2.0

'use strict';

const { once } = require('node:events');
const net = require('node:net');

async function reservePort(): Promise<number> {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { port } = server.address();
  await new Promise<void>((resolve, reject) =>
    server.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function tcpEndpoint(): Promise<string> {
  return `tcp://127.0.0.1:${await reservePort()}`;
}

async function waitUntil(predicate, timeoutMs: number, message: string): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  throw new Error(message);
}

async function waitForConnectionReady(monitor, zlink, timeoutMs = 5000) {
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  const deadline = Date.now() + timeoutMs;
  poller.add(monitor, [zlink.PollEventFlag.PollIn], 1);
  try {
    while (Date.now() < deadline) {
      const remaining = Math.max(1, deadline - Date.now());
      if (poller.wait(events, remaining) === 0) continue;
      for (;;) {
        const event = monitor.recv(zlink.RecvFlags.DontWait);
        if (!event) break;
        if (event.event === zlink.MonitorEventType.ConnectionReady) {
          return event;
        }
      }
    }
  } finally {
    events.close();
    poller.close();
  }
  throw new Error('connection-ready monitor event timed out');
}

function frame(payload) {
  const framed = Buffer.allocUnsafe(payload.length + 6);
  framed.writeUInt16BE(0, 0);
  framed.writeUInt32BE(payload.length, 2);
  payload.copy(framed, 6);
  return framed;
}

module.exports = {
  frame,
  reservePort,
  tcpEndpoint,
  waitForConnectionReady,
  waitUntil
};
