// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const net = require('node:net');
const path = require('node:path');
const zlink = require('@zlink-systems/zlink');
async function tcpEndpoint() {
    const server = net.createServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const address = server.address();
    const port = typeof address === 'object' && address ? address.port : 0;
    await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
    return `tcp://127.0.0.1:${port}`;
}
function waitForMonitorEvent(poller, events, monitor, expectedEvent, expectedSlot, timeoutMs = 5_000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        const remaining = Math.max(1, deadline - Date.now());
        if (poller.wait(events, remaining) === 0)
            continue;
        assert.equal(events.sourceKind(0), 1);
        assert.strictEqual(events.source(0), monitor);
        assert.equal(events.slot(0), expectedSlot);
        assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollIn), true);
        for (;;) {
            const event = monitor.recv(zlink.RecvFlags.DontWait);
            if (!event)
                break;
            if (event.event === expectedEvent)
                return event;
        }
    }
    throw new Error(`monitor event ${expectedEvent} timed out`);
}
test('poller returns a monitor source and DONTWAIT drains ready and disconnected events', async () => {
    const endpoint = await tcpEndpoint();
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    const monitor = dealer.monitorOpen([
        zlink.MonitorEventType.ConnectionReady,
        zlink.MonitorEventType.Disconnected,
    ]);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    try {
        poller.add(monitor, [zlink.PollEventFlag.PollIn], 41);
        router.bind(endpoint);
        dealer.connect(endpoint);
        waitForMonitorEvent(poller, events, monitor, zlink.MonitorEventType.ConnectionReady, 41);
        poller.modify(monitor, [zlink.PollEventFlag.PollIn]);
        router.close();
        waitForMonitorEvent(poller, events, monitor, zlink.MonitorEventType.Disconnected, 41);
    }
    finally {
        events.close();
        poller.close();
        monitor.close();
        dealer.close();
        router.close();
        ctx.close();
    }
});
test('removing a monitor suppresses poll delivery without consuming its event', async () => {
    const endpoint = await tcpEndpoint();
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    const monitor = dealer.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    try {
        poller.add(monitor, [zlink.PollEventFlag.PollIn], 42);
        assert.equal(poller.remove(monitor), true);
        router.bind(endpoint);
        dealer.connect(endpoint);
        assert.equal(poller.wait(events, 250), 0);
        const event = monitor.recv(zlink.RecvFlags.None);
        assert.equal(event?.event, zlink.MonitorEventType.ConnectionReady);
    }
    finally {
        events.close();
        poller.close();
        monitor.close();
        dealer.close();
        router.close();
        ctx.close();
    }
});
test('monitor reports the Core result for unsupported poll events', () => {
    const ctx = zlink.createContext();
    const dealer = zlink.createDealerSocket(ctx);
    const monitor = dealer.monitorOpen();
    const poller = zlink.createPoller();
    const isNotSupported = (error) => error instanceof zlink.ConfigError
        && error.result === zlink.ConfigResult.NotSupported
        && error.nativeErrno === 95;
    try {
        assert.throws(() => poller.add(monitor, [zlink.PollEventFlag.PollPri], 43), isNotSupported);
        poller.add(monitor, [zlink.PollEventFlag.PollIn], 43);
        assert.throws(() => poller.modify(monitor, [zlink.PollEventFlag.PollPri]), isNotSupported);
        assert.equal(poller.size, 1);
    }
    finally {
        poller.close();
        monitor.close();
        dealer.close();
        ctx.close();
    }
});
test('native wait passes an empty event buffer to Core', () => {
    const native = require(path.resolve(__dirname, '../../build/Release/zlink.node'));
    const poller = zlink.createPoller();
    try {
        assert.throws(() => native.pollerWaitInto(poller._native, null, 0, 0), (error) => error instanceof Error
            && !(error instanceof RangeError)
            && error.nativeResult === zlink.ConfigResult.InvalidArgument
            && error.nativeErrno === 22);
    }
    finally {
        poller.close();
    }
});
