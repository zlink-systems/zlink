// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const { once } = require('node:events');
const net = require('node:net');
async function reservePort() {
    const server = net.createServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const { port } = server.address();
    await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
    return port;
}
async function tcpEndpoint() {
    return `tcp://127.0.0.1:${await reservePort()}`;
}
async function waitUntil(predicate, timeoutMs, message) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (predicate())
            return;
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error(message);
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
    waitUntil
};
