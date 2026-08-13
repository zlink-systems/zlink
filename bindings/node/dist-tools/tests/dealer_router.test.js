'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
test('dealer/router uses routing id through Received and routed send', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-contract');
    dealer.connect('inproc://dealer-router-contract');
    dealer.send().message('hello').submit();
    const request = new zlink.Received();
    router.recv(request);
    assert.equal(request.parts.length, 1);
    assert.ok(Object.isFrozen(request.parts));
    assert.equal(request.parts[0].data().toString(), 'hello');
    assert.ok(request.routingId instanceof zlink.RoutingId);
    assert.equal(request.parts[0].refCount(), 1);
    assert.notEqual(request.parts[0].getProperty('Routing-Id'), null);
    assert.equal(request.parts[0].getProperty('Routing-Id'), request.parts[0].getProperty('Identity'));
    router.send(request.routingId).message('world').submit();
    const response = new zlink.Received();
    dealer.recv(response);
    assert.equal(response.parts.length, 1);
    assert.ok(Object.isFrozen(response.parts));
    assert.equal(response.parts[0].data().toString(), 'world');
    assert.equal(typeof router.reply, 'function');
    dealer.close();
    router.close();
    ctx.close();
});
test('router recv fills caller-provided Received with routing metadata', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-recv-payload-into');
    dealer.connect('inproc://dealer-router-recv-payload-into');
    dealer.send().message(Buffer.from('payload')).submit();
    const received = new zlink.Received();
    assert.equal(router.recv(received), true);
    assert.ok(received.routingId);
    assert.equal(received.requestSeq, null);
    assert.equal(received.singlePartOrThrow().data().toString(), 'payload');
    dealer.close();
    router.close();
    ctx.close();
});
test('router forwards an unread received Message from managed storage', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-managed-forward');
    dealer.connect('inproc://dealer-router-managed-forward');
    dealer.send().message(Buffer.alloc(4096, 0x61)).submit();
    const received = new zlink.Received();
    assert.equal(router.recv(received), true);
    assert.ok(received.routingId);
    // Forward the JS-owned receive Buffer without reading it first. Submit
    // materializes the Core frame while preserving the public move contract.
    router.send(received.routingId).message(received.singlePartOrThrow()).submit();
    assert.equal(received.singlePartOrThrow().size(), 0);
    const echoed = new zlink.Received();
    assert.equal(dealer.recv(echoed), true);
    assert.equal(echoed.singlePartOrThrow().size(), 4096);
    assert.equal(echoed.singlePartOrThrow().getString('utf8').slice(0, 1), 'a');
    echoed.close();
    received.close();
    assert.doesNotThrow(() => received.close());
    dealer.close();
    router.close();
    ctx.close();
});
test('router repeatedly transfers pooled native receive frames without stale state', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-native-frame-reuse');
    dealer.connect('inproc://dealer-router-native-frame-reuse');
    const received = new zlink.Received();
    const echoed = new zlink.Received();
    for (let index = 0; index < 256; index += 1) {
        const expected = `payload-${index}`;
        dealer.send().message(expected).submit();
        assert.equal(router.recv(received), true);
        assert.ok(received.routingId);
        // Forward without data(): successful submit moves this exact native frame
        // to Core. The next recv may reuse its storage only after that ownership
        // transfer has completed and the public Message has been consumed.
        router.send(received.routingId)
            .message(received.singlePartOrThrow())
            .submit();
        assert.equal(dealer.recv(echoed), true);
        assert.equal(echoed.singlePartOrThrow().getString('utf8'), expected);
    }
    echoed.close();
    received.close();
    dealer.close();
    router.close();
    ctx.close();
});
test('router nonblocking recv returns false without data', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-recv-payload-into');
    dealer.connect('inproc://dealer-router-recv-payload-into');
    const received = new zlink.Received();
    assert.equal(router.recv(received, zlink.RecvFlags.DontWait), false);
    dealer.close();
    router.close();
    ctx.close();
});
test('router nonblocking recv preserves order and multipart ownership', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-recv-order');
    dealer.connect('inproc://dealer-router-recv-order');
    for (let index = 0; index < 80; index += 1) {
        dealer.send()
            .message(Buffer.from(`header-${index}`))
            .message(Buffer.from(`payload-${index}`))
            .submit();
    }
    for (let index = 0; index < 80; index += 1) {
        const received = new zlink.Received();
        assert.equal(router.recv(received, zlink.RecvFlags.DontWait), true);
        assert.equal(received.parts.length, 2);
        assert.equal(received.parts[0].getString('utf8'), `header-${index}`);
        assert.equal(received.parts[1].getString('utf8'), `payload-${index}`);
        received.close();
    }
    const empty = new zlink.Received();
    assert.equal(router.recv(empty, zlink.RecvFlags.DontWait), false);
    empty.close();
    dealer.close();
    router.close();
    ctx.close();
});
