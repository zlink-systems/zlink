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
test('stream recv uses the common nonblocking receive path', async () => {
    const port = await reserveTcpPort();
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    let client = null;
    const received = new zlink.Received();
    try {
        stream.bind(`tcp://127.0.0.1:${port}`);
        client = net.createConnection({ host: '127.0.0.1', port });
        await once(client, 'connect');
        client.write(Buffer.from('stream-recv-path'));
        let receivedMessage = false;
        for (let attempt = 0; attempt < 100; attempt += 1) {
            if (stream.recv(received, zlink.RecvFlags.DontWait)) {
                receivedMessage = true;
                break;
            }
            await new Promise((resolve) => setTimeout(resolve, 5));
        }
        assert.equal(receivedMessage, true);
        assert.equal(received.singlePartOrThrow().data().toString(), 'stream-recv-path');
    }
    finally {
        received.close();
        client?.destroy();
        stream.close();
        ctx.close();
    }
});
test('stream packet body materialization defaults to native and locks on registration', () => {
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    try {
        assert.equal(stream.options.packetBodyMaterialization, zlink.StreamPacketBodyMaterialization.Native);
        stream.options.packetBodyMaterialization =
            zlink.StreamPacketBodyMaterialization.Managed;
        assert.equal(stream.options.packetBodyMaterialization, zlink.StreamPacketBodyMaterialization.Managed);
        stream.setPacketHandler((_sourceRid, header, body) => {
            header.close();
            body.close();
        });
        assert.throws(() => {
            stream.options.packetBodyMaterialization =
                zlink.StreamPacketBodyMaterialization.Native;
        }, /cannot change after packet handler registration/);
    }
    finally {
        stream.close();
        ctx.close();
    }
});
test('stream packet body can use managed Buffer materialization', async () => {
    const port = await reserveTcpPort();
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    let client = null;
    const headerBytes = Buffer.from('managed-header');
    const bodyBytes = Buffer.alloc(4096, 0x37);
    const expected = packetFrame(headerBytes, bodyBytes);
    try {
        stream.options.packetBodyMaterialization =
            zlink.StreamPacketBodyMaterialization.Managed;
        stream.bind(`tcp://127.0.0.1:${port}`);
        client = net.createConnection({ host: '127.0.0.1', port });
        await once(client, 'connect');
        const forwarded = new Promise((resolve, reject) => {
            stream.setPacketHandler((sourceRid, header, body) => {
                try {
                    assert.deepEqual(header.data(), headerBytes);
                    assert.deepEqual(body.data(), bodyBytes);
                    const prefix = Buffer.allocUnsafe(6);
                    prefix.writeUInt16BE(header.size(), 0);
                    prefix.writeUInt32BE(body.size(), 2);
                    assert.equal(stream.trySend(sourceRid)
                        .message(prefix)
                        .message(header)
                        .message(body)
                        .flags(zlink.SendFlags.DontWait)
                        .submit(), true);
                    assert.equal(body.size(), 0);
                    header.close();
                    body.close();
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
                    const submitted = stream.trySend(sourceRid)
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
