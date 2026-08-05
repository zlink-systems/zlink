// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
test('router can send reply in a synchronous request-reply exchange', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-callback');
    dealer.connect('inproc://dealer-router-callback');
    dealer.send().message('request').submit();
    const request = new zlink.Received();
    router.recv(request);
    assert.ok(request.routingId instanceof zlink.RoutingId);
    assert.equal(request.parts[0].data().toString(), 'request');
    router.send(request.routingId).message('reply').submit();
    const response = new zlink.Received();
    dealer.recv(response);
    assert.equal(response.parts[0].data().toString(), 'reply');
    dealer.close();
    router.close();
    ctx.close();
});
test('router can send multiple replies in a synchronous request-reply loop', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-callback-multi');
    dealer.connect('inproc://dealer-router-callback-multi');
    const ROUND_COUNT = 5;
    let roundsCompleted = 0;
    for (let i = 0; i < ROUND_COUNT; i += 1) {
        dealer.send().message(`request-${i}`).submit();
        const request = new zlink.Received();
        router.recv(request);
        assert.ok(request.routingId instanceof zlink.RoutingId);
        assert.equal(request.parts[0].data().toString(), `request-${i}`);
        router.send(request.routingId).message(`reply-${i}`).submit();
        const reply = new zlink.Received();
        dealer.recv(reply);
        assert.equal(reply.parts[0].data().toString(), `reply-${i}`);
        roundsCompleted += 1;
    }
    assert.equal(roundsCompleted, ROUND_COUNT);
    dealer.close();
    router.close();
    ctx.close();
});
test('router recv + send works as synchronous request-reply', () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    router.bind('inproc://dealer-router-sync-rr');
    dealer.connect('inproc://dealer-router-sync-rr');
    dealer.send().message('sync-request').submit();
    const request = new zlink.Received();
    router.recv(request);
    assert.ok(request.routingId instanceof zlink.RoutingId);
    assert.equal(request.parts[0].data().toString(), 'sync-request');
    router.send(request.routingId).message('sync-reply').submit();
    const response = new zlink.Received();
    dealer.recv(response);
    assert.equal(response.parts[0].data().toString(), 'sync-reply');
    dealer.close();
    router.close();
    ctx.close();
});
