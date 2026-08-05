// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const { tcpEndpoint } = require('./sample_support');
async function main() {
    const endpoint = await tcpEndpoint();
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
    }
    finally {
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
