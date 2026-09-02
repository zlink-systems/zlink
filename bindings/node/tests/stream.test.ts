// SPDX-License-Identifier: MPL-2.0

'use strict';

const net = require('node:net');
const { once } = require('node:events');
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

async function reserveTcpPort(): Promise<number> {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const address = server.address();
  assert.ok(address && typeof address !== 'string');
  const port = address.port;
  await new Promise<void>((resolve) => server.close(() => resolve()));
  return port;
}

function packetFrame(header: Buffer, body: Buffer): Buffer {
  const frame = Buffer.allocUnsafe(6 + header.length + body.length);
  frame.writeUInt16BE(header.length, 0);
  frame.writeUInt32BE(body.length, 2);
  header.copy(frame, 6);
  body.copy(frame, 6 + header.length);
  return frame;
}

async function receivePacket(stream: any, packet: any): Promise<void> {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    if (stream.recvPacket(packet, zlink.RecvFlags.DontWait)) return;
    await new Promise((resolve) => setTimeout(resolve, 2));
  }
  throw new Error('packet receive timed out');
}

test('STREAM RAW receive requires explicit recvMode', async () => {
  const port = await reserveTcpPort();
  const ctx = zlink.createContext();
  const stream = zlink.createStreamSocket(ctx);
  const received = new zlink.Received();
  let client: import('node:net').Socket | null = null;
  try {
    assert.equal(stream.options.recvMode, zlink.StreamRecvMode.Unspecified);
    stream.options.recvMode = zlink.StreamRecvMode.Raw;
    stream.bind(`tcp://127.0.0.1:${port}`);
    client = net.createConnection({ host: '127.0.0.1', port });
    await once(client, 'connect');
    client.write('raw-data');
    for (let attempt = 0; attempt < 200; attempt += 1) {
      if (stream.recv(received, zlink.RecvFlags.DontWait)) break;
      await new Promise((resolve) => setTimeout(resolve, 2));
    }
    assert.equal(received.parts[0].getString(), 'raw-data');
  } finally {
    received.close(); client?.destroy(); stream.close(); ctx.close();
  }
});

test('reusable StreamPacket resets on no-data and refills on reuse', async () => {
  const port = await reserveTcpPort();
  const ctx = zlink.createContext();
  const stream = zlink.createStreamSocket(ctx);
  const packet = new zlink.StreamPacket();
  let client: import('node:net').Socket | null = null;
  try {
    assert.throws(() => { stream.options.recvMode = zlink.StreamRecvMode.Unspecified; }, RangeError);
    stream.options.recvMode = zlink.StreamRecvMode.Packet;
    stream.bind(`tcp://127.0.0.1:${port}`);
    client = net.createConnection({ host: '127.0.0.1', port });
    await once(client, 'connect');

    client.write(packetFrame(Buffer.from('h1'), Buffer.from('b1')));
    await receivePacket(stream, packet);
    assert.equal(packet.isEmpty, false);
    assert.equal(packet.header.getString(), 'h1');
    assert.equal(packet.body.getString(), 'b1');

    assert.equal(stream.recvPacket(packet, zlink.RecvFlags.DontWait), false);
    assert.equal(packet.isEmpty, true);
    assert.equal(packet.routingId, null);

    client.write(packetFrame(Buffer.from('h2'), Buffer.from('b2')));
    await receivePacket(stream, packet);
    assert.equal(packet.header.getString(), 'h2');
    assert.equal(packet.body.getString(), 'b2');
    stream.close();
    assert.throws(() => stream.recvPacket(packet, zlink.RecvFlags.DontWait));
    assert.equal(packet.isEmpty, true);
  } finally {
    packet.close(); client?.destroy(); stream.close(); ctx.close();
  }
});

test('STREAM packet target can be captured for managed send', async () => {
  const port = await reserveTcpPort();
  const ctx = zlink.createContext();
  const stream = zlink.createStreamSocket(ctx);
  const packet = new zlink.StreamPacket();
  let client: import('node:net').Socket | null = null;
  try {
    stream.options.recvMode = zlink.StreamRecvMode.Packet;
    stream.bind(`tcp://127.0.0.1:${port}`);
    client = net.createConnection({ host: '127.0.0.1', port });
    await once(client, 'connect');
    client.write(packetFrame(Buffer.from('header'), Buffer.from('body')));
    await receivePacket(stream, packet);
    const operation = stream.send(packet.routingId).message('echo');
    packet.close();
    const echoed = once(client, 'data');
    await operation.submit();
    assert.equal((await echoed)[0].toString(), 'echo');
  } finally {
    packet.close(); client?.destroy(); stream.close(); ctx.close();
  }
});
