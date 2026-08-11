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
