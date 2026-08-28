// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const { waitForConnectionReady } = require('../perf/multi/perf_multi_runtime');
const { benchmarkEndpoint } = require('../perf/common/perf_endpoint');
const { configureTlsClient, configureTlsServer } = require('../perf/common/perf_tls');
function nextTurn() {
    return new Promise((resolve) => setImmediate(resolve));
}
function within(promise, timeoutMs = 5_000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`routed SENDSEND contract timed out after ${timeoutMs}ms`)), timeoutMs);
        promise.then((value) => { clearTimeout(timer); resolve(value); }, (error) => { clearTimeout(timer); reject(error); });
    });
}
function advanceReceived(received, mode) {
    if (mode === 'reuse') {
        return received;
    }
    received.close();
    return mode === 'close-fresh' ? new zlink.Received() : received;
}
async function runRoutedSendSendContract({ transport, clientCount, sendsPerClient, payloadSize, receiveMode = 'reuse' }) {
    const serverContext = zlink.createContext();
    const clientContext = zlink.createContext();
    serverContext.options.ioThreads = 4;
    clientContext.options.ioThreads = 4;
    serverContext.options.autoHwmEnabled = true;
    clientContext.options.autoHwmEnabled = true;
    const router = zlink.createRouterSocket(serverContext);
    const dealers = Array.from({ length: clientCount }, () => zlink.createDealerSocket(clientContext));
    let serverReceived = new zlink.Received();
    const clientReceived = dealers.map(() => new zlink.Received());
    const expected = dealers.length * sendsPerClient;
    const pendingReplies = [];
    try {
        const endpoint = await benchmarkEndpoint(transport, `routed-sendsend-contract-${process.pid}`, { suite: 'multi' });
        configureTlsServer(router, transport);
        router.bind(endpoint);
        await Promise.all(dealers.map((dealer, index) => {
            dealer.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${index}`)));
            configureTlsClient(dealer, transport);
            return waitForConnectionReady(dealer, () => dealer.connect(endpoint));
        }));
        serverContext.recalculateAutoHwm();
        clientContext.recalculateAutoHwm();
        const payloads = dealers.map(() => Buffer.alloc(payloadSize));
        const senders = dealers.map(async (dealer, clientIndex) => {
            for (let sequence = 0; sequence < sendsPerClient; sequence += 1) {
                const payload = payloads[clientIndex];
                payload.fill(clientIndex + sequence);
                await dealer.send().message(payload).message(Buffer.alloc(0)).submit();
                await nextTurn();
            }
        });
        const serverPump = (async () => {
            let receivedCount = 0;
            while (receivedCount < expected) {
                while (router.recv(serverReceived, zlink.RecvFlags.DontWait)) {
                    assert.ok(serverReceived.routingId);
                    assert.equal(serverReceived.parts.length, 2);
                    assert.equal(serverReceived.parts[0].data().length, payloadSize);
                    assert.equal(serverReceived.parts[1].data().length, 0);
                    const routingId = zlink.RoutingId.from(serverReceived.routingId.toBytes());
                    const parts = serverReceived.parts.map((part) => Buffer.from(part.data()));
                    const reply = router.send(routingId)
                        .message(parts[0])
                        .message(parts[1])
                        .submit();
                    pendingReplies.push(reply);
                    receivedCount += 1;
                    serverReceived = advanceReceived(serverReceived, receiveMode);
                    await nextTurn();
                }
                await nextTurn();
            }
            await Promise.all(pendingReplies);
        })();
        const clientPump = (async () => {
            let replyCount = 0;
            while (replyCount < expected) {
                for (let index = 0; index < dealers.length; index += 1) {
                    while (dealers[index].recv(clientReceived[index], zlink.RecvFlags.DontWait)) {
                        const received = clientReceived[index];
                        assert.equal(received.parts.length, 2);
                        assert.equal(received.parts[0].data().length, payloadSize);
                        assert.equal(received.parts[1].data().length, 0);
                        replyCount += 1;
                        clientReceived[index] = advanceReceived(received, receiveMode);
                        await nextTurn();
                    }
                }
                await nextTurn();
            }
        })();
        await within(Promise.all([...senders, serverPump, clientPump]), 30_000);
    }
    finally {
        serverReceived.close();
        clientReceived.forEach((received) => received.close());
        dealers.forEach((dealer) => dealer.close());
        router.close();
        clientContext.close();
        serverContext.close();
    }
}
test('routed SENDSEND reuses receive envelopes and echoes exactly two application parts', async () => {
    await runRoutedSendSendContract({
        transport: 'tcp',
        clientCount: 4,
        sendsPerClient: 32,
        payloadSize: 64
    });
});
test('routed SENDSEND preserves two-part records under concurrent TCP traffic', async () => {
    await runRoutedSendSendContract({
        transport: 'tcp',
        clientCount: 100,
        sendsPerClient: 512,
        payloadSize: 1024
    });
});
test('routed SENDSEND preserves two-part records under concurrent WSS traffic', async () => {
    await runRoutedSendSendContract({
        transport: 'wss',
        clientCount: 100,
        sendsPerClient: 512,
        payloadSize: 1024
    });
});
test('routed multipart replacement is identical for reuse, close-reuse, and close-fresh', async () => {
    for (const receiveMode of ['reuse', 'close-reuse', 'close-fresh']) {
        await runRoutedSendSendContract({
            transport: 'tcp',
            clientCount: 32,
            sendsPerClient: 256,
            payloadSize: 1024,
            receiveMode
        });
    }
});
