// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
function payloads(count) {
    return Array.from({ length: count }, (_, index) => index === 1 ? Buffer.alloc(0) : Buffer.alloc(1024, index));
}
function withParts(operation, parts) {
    for (const part of parts)
        operation = operation.message(part);
    return operation;
}
test('whole-message PAIR receive grows and reuses capacity without consuming the next record', () => {
    const context = zlink.createContext();
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    const received = new zlink.Received();
    try {
        sender.bind('inproc://whole-message-pair');
        receiver.connect('inproc://whole-message-pair');
        for (const count of [9, 17, 2, 17]) {
            const expected = payloads(count);
            withParts(sender.send(), expected).submit();
            sender.send().message('next').submit();
            assert.equal(receiver.recv(received, zlink.RecvFlags.DontWait), true);
            assert.deepEqual(received.parts.map(part => part.data()), expected);
            const retained = received.parts.map(part => part.copy());
            assert.equal(receiver.recv(received), true);
            assert.equal(received.singlePartOrThrow().getString(), 'next');
            assert.equal(receiver.recv(received, zlink.RecvFlags.DontWait), false);
            assert.equal(received.singlePartOrThrow().getString(), 'next');
            assert.deepEqual(retained.map(part => part.data()), expected);
            retained.forEach(part => { part.close(); part.close(); });
            received.close();
        }
    }
    finally {
        received.close();
        receiver.close();
        sender.close();
        context.close();
    }
});
test('whole-message REQUEST growth preserves the route, reply token, and independent part owners', async () => {
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    const received = new zlink.Received();
    const peer = zlink.RoutingId.from(Buffer.from('whole-message-client'));
    try {
        router.bind('inproc://whole-message-request');
        dealer.setRoutingId(peer);
        dealer.connect('inproc://whole-message-request');
        for (const count of [33, 2]) {
            const expected = payloads(count);
            const pending = withParts(dealer.request(), expected).timeout(1000).submit();
            assert.equal(router.recv(received), true);
            assert.ok(received.routingId.equals(peer));
            assert.ok(received.replyToken);
            assert.equal(received.parts.length, count);
            const retained = received.parts.map(part => part.copy());
            let reply = received.reply();
            const sent = received.parts.slice();
            for (const part of sent)
                reply = reply.message(part);
            reply.submit();
            sent.forEach(part => assert.equal(part.size(), 0));
            received.close();
            const response = await pending;
            assert.deepEqual(response.map(part => part.data()), expected);
            response.forEach(part => { part.close(); part.close(); });
            assert.deepEqual(retained.map(part => part.data()), expected);
            retained.forEach(part => { part.close(); part.close(); });
            await dealer.send().message('data-after-request').submit();
            assert.equal(router.recv(received), true);
            assert.ok(received.routingId.equals(peer));
            assert.equal(received.replyToken, null);
            assert.equal(received.singlePartOrThrow().getString(), 'data-after-request');
            received.close();
        }
    }
    finally {
        received.close();
        dealer.close();
        router.close();
        context.close();
    }
});
test('whole-message SUB grows topic and part buffers while preserving queued record boundaries', () => {
    const context = zlink.createContext();
    const publisher = zlink.createXPubSocket(context);
    const subscriber = zlink.createXSubSocket(context);
    const received = new zlink.TopicMessage();
    try {
        publisher.bind('inproc://whole-message-topic');
        subscriber.connect('inproc://whole-message-topic');
        subscriber.setSubscription('');
        const event = new zlink.SubscriptionEvent();
        assert.equal(publisher.receiveSubscriptionEvent(event), true);
        assert.equal(event.subscribed, true);
        for (const [count, flags] of [[65, zlink.RecvFlags.None], [129, zlink.RecvFlags.DontWait],
            [3, zlink.RecvFlags.None]]) {
            const expected = payloads(count);
            const topic = 'whole-message-' + 't'.repeat(300);
            withParts(publisher.publish(topic), expected).submit();
            publisher.publish('next-topic').message('next').submit();
            assert.equal(subscriber.subscribe(received, flags), true);
            assert.equal(received.topic, topic);
            assert.deepEqual(received.parts.map(part => part.data()), expected);
            const retained = received.parts.map(part => part.copy());
            assert.equal(subscriber.subscribe(received), true);
            assert.equal(received.topic, 'next-topic');
            assert.equal(received.singlePartOrThrow().getString(), 'next');
            assert.equal(subscriber.subscribe(received, zlink.RecvFlags.DontWait), false);
            assert.deepEqual(retained.map(part => part.data()), expected);
            retained.forEach(part => { part.close(); part.close(); });
            received.close();
        }
    }
    finally {
        received.close();
        subscriber.close();
        publisher.close();
        context.close();
    }
});
