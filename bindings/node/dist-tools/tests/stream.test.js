// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const net = require('node:net');
const { once } = require('node:events');
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
async function reserveTcpPort() {
    const reservation = net.createServer();
    reservation.listen(0, '127.0.0.1');
    await once(reservation, 'listening');
    const address = reservation.address();
    assert.ok(address && typeof address !== 'string');
    const port = address.port;
    await new Promise((resolve, reject) => {
        reservation.close((error) => error ? reject(error) : resolve());
    });
    return port;
}
function packetFrame(header, body) {
    const frame = Buffer.allocUnsafe(6 + header.length + body.length);
    frame.writeUInt16BE(header.length, 0);
    frame.writeUInt32BE(body.length, 2);
    header.copy(frame, 6);
    body.copy(frame, 6 + header.length);
    return frame;
}
async function readExactly(socket, size) {
    const chunks = [];
    let received = 0;
    while (received < size) {
        const [chunk] = await once(socket, 'data');
        chunks.push(chunk);
        received += chunk.length;
    }
    return Buffer.concat(chunks, received).subarray(0, size);
}
test('stream recv uses the common nonblocking receive batch path', async () => {
    const port = await reserveTcpPort();
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    let client = null;
    const received = new zlink.Received();
    try {
        stream.bind(`tcp://127.0.0.1:${port}`);
        client = net.createConnection({ host: '127.0.0.1', port });
        await once(client, 'connect');
        client.write(Buffer.from('stream-batch-path'));
        let receivedMessage = false;
        for (let attempt = 0; attempt < 100; attempt += 1) {
            if (stream.recv(received, zlink.RecvFlags.DontWait)) {
                receivedMessage = true;
                break;
            }
            await new Promise((resolve) => setTimeout(resolve, 5));
        }
        assert.equal(receivedMessage, true);
        assert.equal(received.singlePartOrThrow().data().toString(), 'stream-batch-path');
    }
    finally {
        received.close();
        client?.destroy();
        stream.close();
        ctx.close();
    }
});
test('stream packet body forwards from native storage without JavaScript materialization', async () => {
    const port = await reserveTcpPort();
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    let client = null;
    const headerBytes = Buffer.from('relay-header');
    const bodyBytes = Buffer.alloc(64 * 1024, 0x5a);
    const expected = packetFrame(headerBytes, bodyBytes);
    try {
        stream.bind(`tcp://127.0.0.1:${port}`);
        client = net.createConnection({ host: '127.0.0.1', port });
        await once(client, 'connect');
        const forwarded = new Promise((resolve, reject) => {
            stream.setPacketHandler((sourceRid, header, body) => {
                try {
                    const prefix = Buffer.allocUnsafe(6);
                    prefix.writeUInt16BE(header.size(), 0);
                    prefix.writeUInt32BE(body.size(), 2);
                    const submitted = stream.send(sourceRid)
                        .message(prefix)
                        .message(header)
                        .message(body)
                        .flags(zlink.SendFlags.DontWait)
                        .submit();
                    assert.equal(submitted, true);
                    assert.equal(body.size(), 0, 'successful native forwarding must consume the body');
                    assert.doesNotThrow(() => body.close());
                    assert.doesNotThrow(() => body.close());
                    header.close();
                    resolve();
                }
                catch (error) {
                    header.close();
                    body.close();
                    reject(error);
                }
            });
        });
        const echoed = readExactly(client, expected.length);
        client.write(expected);
        await forwarded;
        assert.deepEqual(await echoed, expected);
    }
    finally {
        client?.destroy();
        stream.close();
        ctx.close();
    }
});
test('stream packet body remains owned when native forwarding is backpressured', async () => {
    const port = await reserveTcpPort();
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    let client = null;
    const bodyBytes = Buffer.alloc(65536, 0x42);
    const inbound = packetFrame(Buffer.alloc(0), bodyBytes);
    try {
        stream.options.sendHwm = 1n;
        stream.bind(`tcp://127.0.0.1:${port}`);
        client = net.createConnection({ host: '127.0.0.1', port });
        await once(client, 'connect');
        client.pause();
        const backpressured = new Promise((resolve, reject) => {
            let attempts = 0;
            const timeout = setTimeout(() => reject(new Error(`stream send did not reach backpressure (${attempts} attempts)`)), 10000);
            stream.setPacketHandler((sourceRid, header, body) => {
                try {
                    attempts += 1;
                    const prefix = Buffer.allocUnsafe(6);
                    prefix.writeUInt16BE(header.size(), 0);
                    prefix.writeUInt32BE(body.size(), 2);
                    const submitted = stream.send(sourceRid)
                        .message(prefix)
                        .message(header)
                        .message(body)
                        .flags(zlink.SendFlags.DontWait)
                        .submit();
                    if (!submitted) {
                        assert.equal(body.size(), bodyBytes.length);
                        body.close();
                        header.close();
                        clearTimeout(timeout);
                        resolve();
                        return;
                    }
                    body.close();
                    header.close();
                    if (attempts >= 128) {
                        clearTimeout(timeout);
                        reject(new Error('stream send stayed writable above its configured HWM'));
                        return;
                    }
                }
                catch (error) {
                    header.close();
                    body.close();
                    clearTimeout(timeout);
                    reject(error);
                }
            });
        });
        client.write(Buffer.concat(Array.from({ length: 128 }, () => inbound)));
        await backpressured;
    }
    finally {
        client?.destroy();
        stream.close();
        ctx.close();
    }
});
