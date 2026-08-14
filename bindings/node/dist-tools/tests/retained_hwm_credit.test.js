'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const { once } = require('node:events');
const zlink = require('@zlink-systems/zlink');
let endpointSequence = 0;
function endpoint(label) {
    endpointSequence += 1;
    return `inproc://node-retained-${label}-${process.pid}-${endpointSequence}`;
}
async function reserveTcpPort() {
    const reservation = net.createServer();
    reservation.listen(0, '127.0.0.1');
    await once(reservation, 'listening');
    const address = reservation.address();
    assert.ok(address && typeof address !== 'string');
    const port = address.port;
    await new Promise((resolve, reject) => {
        reservation.close((error) => error ? reject(error) : resolve(undefined));
    });
    return port;
}
test('explicit retained recv owns Core credit while ordinary recv returns it immediately', () => {
    const ctx = zlink.createContext();
    ctx.options.autoHwmEnabled = false;
    const receiver = zlink.createPairSocket(ctx);
    const sender = zlink.createPairSocket(ctx);
    receiver.options.recvHwm = 4096n;
    sender.options.sendHwm = 4096n;
    const address = endpoint('pair');
    receiver.bind(address);
    sender.connect(address);
    sender.send().message(Buffer.alloc(1_024, 0x6a)).submit();
    const queued = ctx.getCoreHwmBudgetSnapshot();
    assert.ok(queued.coreQueueAccountedBytes > 0n);
    assert.equal(queued.applicationAccountedBytes, 0n);
    const retained = new zlink.Received();
    assert.equal(receiver.recvRetained(retained), true);
    assert.equal(retained.singlePartOrThrow().size(), 1_024);
    let snapshot = ctx.getCoreHwmBudgetSnapshot();
    assert.equal(snapshot.coreQueueAccountedBytes, 0n);
    assert.equal(snapshot.applicationAccountedBytes, queued.coreQueueAccountedBytes);
    assert.equal(snapshot.currentAccountedBytes, queued.currentAccountedBytes);
    assert.equal(snapshot.outstandingApplicationLeaseCount, 1n);
    assert.equal(Object.hasOwn(retained, 'hwmBudgetLeaseOwner'), false);
    const retainedPayload = retained.singlePartOrThrow().data();
    assert.equal(Object.hasOwn(retainedPayload, 'hwmBudgetLeaseOwner'), false);
    retained.close();
    snapshot = ctx.getCoreHwmBudgetSnapshot();
    assert.equal(snapshot.applicationAccountedBytes, 0n);
    assert.equal(snapshot.outstandingApplicationLeaseCount, 0n);
    assert.equal(retainedPayload.length, 1_024);
    sender.send().message('ordinary').submit();
    const ordinary = new zlink.Received();
    assert.equal(receiver.recv(ordinary), true);
    assert.equal(ordinary.singlePartOrThrow().getString(), 'ordinary');
    snapshot = ctx.getCoreHwmBudgetSnapshot();
    assert.equal(snapshot.applicationAccountedBytes, 0n);
    assert.equal(snapshot.outstandingApplicationLeaseCount, 0n);
    ordinary.close();
    receiver.close();
    sender.close();
    ctx.close();
});
test('retained aggregate releases its retired origin after socket close', () => {
    const ctx = zlink.createContext();
    const receiver = zlink.createPairSocket(ctx);
    const sender = zlink.createPairSocket(ctx);
    const address = endpoint('retired-origin');
    receiver.bind(address);
    sender.connect(address);
    sender.send().message('retired').submit();
    const retained = new zlink.Received();
    assert.equal(receiver.recvRetained(retained), true);
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 1n);
    receiver.close();
    sender.close();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 1n);
    retained.close();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    ctx.shutdown();
    ctx.close();
});
test('stream retained recv preserves its source route and Core credit', async () => {
    const port = await reserveTcpPort();
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    const retained = new zlink.Received();
    let client = null;
    try {
        stream.bind(`tcp://127.0.0.1:${port}`);
        client = net.createConnection({ host: '127.0.0.1', port });
        await once(client, 'connect');
        client.write(Buffer.from('stream-retained'));
        let received = false;
        for (let attempt = 0; attempt < 100; attempt += 1) {
            if (stream.recvRetained(retained, zlink.RecvFlags.DontWait)) {
                received = true;
                break;
            }
            await new Promise((resolve) => setTimeout(resolve, 5));
        }
        assert.equal(received, true);
        assert.ok(retained.routingId instanceof zlink.RoutingId);
        assert.equal(retained.singlePartOrThrow().getString(), 'stream-retained');
        assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 1n);
        retained.close();
        assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    }
    finally {
        retained.close();
        client?.destroy();
        stream.close();
        ctx.close();
    }
});
test('retained recv replacement releases the previous aggregate exactly once', () => {
    const ctx = zlink.createContext();
    const receiver = zlink.createPairSocket(ctx);
    const sender = zlink.createPairSocket(ctx);
    const address = endpoint('reuse');
    receiver.bind(address);
    sender.connect(address);
    const result = new zlink.Received();
    sender.send().message('first').submit();
    assert.equal(receiver.recvRetained(result), true);
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 1n);
    sender.send().message('second').submit();
    assert.equal(receiver.recvRetained(result), true);
    assert.equal(result.singlePartOrThrow().getString(), 'second');
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 1n);
    sender.send().message('ordinary replacement').submit();
    assert.equal(receiver.recv(result), true);
    assert.equal(result.singlePartOrThrow().getString(), 'ordinary replacement');
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    result.close();
    result.close();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    const empty = new zlink.Received();
    assert.equal(receiver.recvRetained(empty, zlink.RecvFlags.DontWait), false);
    empty.close();
    receiver.close();
    sender.close();
    ctx.close();
});
test('dealer and router retained recv preserve typed metadata and multipart leases', async () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    const dealerRid = zlink.RoutingId.from('node-retained-dealer');
    dealer.setRoutingId(dealerRid);
    const address = endpoint('router');
    router.bind(address);
    dealer.connect(address);
    const pendingReply = dealer.request()
        .message('request-head')
        .message('request-body')
        .timeout(2_000)
        .submit();
    await new Promise((resolve) => setImmediate(resolve));
    const request = new zlink.Received();
    assert.equal(router.recvRetained(request), true);
    assert.equal(request.routingId.toBytes().toString(), 'node-retained-dealer');
    assert.ok(request.requestSeq > 0n);
    assert.deepEqual(request.parts.map((part) => part.getString()), [
        'request-head',
        'request-body'
    ]);
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 2n);
    request.reply().message('reply').submit();
    request.close();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    // Request progress intentionally uses an unref'ed interval so a pending
    // request never keeps an application alive by itself. Keep this test alive
    // until the native callback settles the Promise.
    const keepAlive = setInterval(() => { }, 10);
    let replyParts;
    try {
        replyParts = await pendingReply;
        assert.equal(replyParts[0].getString(), 'reply');
    }
    finally {
        clearInterval(keepAlive);
        if (replyParts) {
            for (const part of replyParts)
                part.close();
        }
    }
    await dealer.send().message('ready').submit();
    const ready = new zlink.Received();
    router.recv(ready);
    const peerRid = ready.routingId;
    ready.close();
    await router.send(peerRid).message('dealer-payload').submit();
    const dealerReceived = new zlink.Received();
    assert.equal(dealer.recvRetained(dealerReceived), true);
    assert.equal(dealerReceived.requestSeq, null);
    assert.equal(dealerReceived.singlePartOrThrow().getString(), 'dealer-payload');
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 1n);
    dealerReceived.close();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    const dealerRequestCompletion = router.request(peerRid)
        .message('dealer-request-head')
        .message('dealer-request-body')
        .timeout(50)
        .submit();
    await new Promise((resolve) => setImmediate(resolve));
    const dealerRequest = new zlink.Received();
    assert.equal(dealer.recvRetained(dealerRequest), true);
    assert.ok(dealerRequest.requestSeq > 0n);
    assert.deepEqual(dealerRequest.parts.map((part) => part.getString()), [
        'dealer-request-head',
        'dealer-request-body'
    ]);
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 2n);
    dealerRequest.close();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    const dealerRequestKeepAlive = setInterval(() => { }, 10);
    try {
        await assert.rejects(dealerRequestCompletion, (error) => error instanceof zlink.RequestError
            && error.result === zlink.RequestResult.TimedOut);
    }
    finally {
        clearInterval(dealerRequestKeepAlive);
    }
    dealer.close();
    router.close();
    ctx.close();
});
test('explicit retained subscribe preserves topic and one lease per payload part', () => {
    const ctx = zlink.createContext();
    const publisher = zlink.createXPubSocket(ctx);
    const subscriber = zlink.createSubSocket(ctx);
    const address = endpoint('subscribe');
    publisher.bind(address);
    subscriber.connect(address);
    const topic = 'orders';
    subscriber.setSubscription('orders');
    const subscription = new zlink.SubscriptionEvent();
    assert.equal(publisher.receiveSubscriptionEvent(subscription), true);
    publisher.publish(topic).message('alpha').message('beta').submit();
    const retained = new zlink.TopicMessage();
    assert.equal(subscriber.subscribeRetained(retained), true);
    assert.equal(retained.topic, topic);
    assert.deepEqual(retained.parts.map((part) => part.getString()), ['alpha', 'beta']);
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 2n);
    retained.close();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    publisher.publish(topic).message('ordinary').submit();
    const ordinary = new zlink.TopicMessage();
    assert.equal(subscriber.subscribe(ordinary), true);
    assert.equal(ordinary.singlePartOrThrow().getString(), 'ordinary');
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    ordinary.close();
    subscriber.close();
    publisher.close();
    ctx.close();
});
test('abandoned retained aggregate releases Core credit through the native finalizer', {
    skip: typeof global.gc !== 'function'
}, async () => {
    const ctx = zlink.createContext();
    const receiver = zlink.createPairSocket(ctx);
    const sender = zlink.createPairSocket(ctx);
    const address = endpoint('finalizer');
    receiver.bind(address);
    sender.connect(address);
    sender.send().message('abandoned').submit();
    let retainedPayload;
    (() => {
        const abandoned = new zlink.Received();
        assert.equal(receiver.recvRetained(abandoned), true);
        retainedPayload = abandoned.singlePartOrThrow().data();
    })();
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 1n);
    for (let attempt = 0; attempt < 50; attempt += 1) {
        global.gc();
        await new Promise((resolve) => setImmediate(resolve));
        if (ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount === 0n) {
            break;
        }
    }
    assert.equal(ctx.getCoreHwmBudgetSnapshot().outstandingApplicationLeaseCount, 0n);
    assert.equal(retainedPayload.toString(), 'abandoned');
    receiver.close();
    sender.close();
    ctx.close();
});
