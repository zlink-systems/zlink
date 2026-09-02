// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const assert = require('node:assert/strict');
const { once } = require('node:events');
const net = require('node:net');
const zlink = require('@zlink-systems/zlink');
const { reservePort } = require('./sample_support');
async function main() {
    // --8<-- [start:doc]
    const port = await reservePort();
    const endpoint = `tcp://127.0.0.1:${port}`;
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    let client;
    try {
        stream.options.recvMode = zlink.StreamRecvMode.Raw;
        stream.bind(endpoint);
        client = net.createConnection({ host: '127.0.0.1', port });
        await once(client, 'connect');
        const sent = 'hello-stream';
        client.write(Buffer.from(sent));
        const received = new zlink.Received();
        assert.equal(stream.recv(received), true);
        try {
            assert.ok(received.routingId instanceof zlink.RoutingId);
            const recv = received.parts[0].data().toString();
            assert.equal(recv, sent);
            await received.send().message(Buffer.from(sent)).submit();
            const [reply] = await once(client, 'data');
            assert.equal(reply.toString(), sent);
            console.log(`[stream/recv] send: "${sent}" \u2192 recv: "${recv}"`);
        }
        finally {
            received.close();
        }
    }
    finally {
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
