'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
test('request-reply helpers expose canonical socket accessors', () => {
    const ctx = zlink.createContext();
    const routerSocket = zlink.createRouterSocket(ctx);
    const dealerSocket = zlink.createDealerSocket(ctx);
    assert.equal(typeof routerSocket.request, 'function');
    assert.equal(typeof routerSocket.reply, 'function');
    assert.equal(typeof routerSocket.recv, 'function');
    assert.equal(routerSocket.onReceive, undefined);
    assert.equal(typeof dealerSocket.request, 'function');
    assert.equal(typeof dealerSocket.recv, 'function');
    assert.equal(dealerSocket.onReceive, undefined);
    dealerSocket.close();
    routerSocket.close();
    ctx.close();
});
test('router recv and reply still work through the canonical socket surface', () => {
    const ctx = zlink.createContext();
    const routerSocket = zlink.createRouterSocket(ctx);
    const dealerSocket = zlink.createDealerSocket(ctx);
    const clientRoutingId = zlink.RoutingId.from(Buffer.from('request-reply-client'));
    routerSocket.bind('inproc://request-reply-contract');
    dealerSocket.setRoutingId(clientRoutingId);
    dealerSocket.connect('inproc://request-reply-contract');
    dealerSocket.send().message('ping').submit();
    const request = new zlink.Received();
    routerSocket.recv(request);
    assert.ok(request.routingId instanceof zlink.RoutingId);
    assert.equal(request.routingId.toBytes().toString(), 'request-reply-client');
    assert.equal(request.requestSeq, null);
    assert.equal(request.parts[0].data().toString(), 'ping');
    routerSocket.send(request.routingId).message('pong').submit();
    const reply = new zlink.Received();
    dealerSocket.recv(reply);
    assert.equal(reply.parts[0].data().toString(), 'pong');
    assert.equal(reply.requestSeq, null);
    dealerSocket.close();
    routerSocket.close();
    ctx.close();
});
test('router reply rejects unsupported non-none flags', () => {
    const ctx = zlink.createContext();
    const routerSocket = zlink.createRouterSocket(ctx);
    const routingId = zlink.RoutingId.from(Buffer.from('peer'));
    assert.throws(() => routerSocket.reply(routingId, 1n).message('pong').flags(zlink.SendFlags.DontWait).submit(), (error) => error instanceof zlink.SubmitError && error.result === zlink.SubmitResult.NotSupported);
    routerSocket.close();
    ctx.close();
});
test('completion control progresses without consuming application receive', async () => {
    const ctx = zlink.createContext();
    const server = zlink.createRouterSocket(ctx);
    const client = zlink.createRouterSocket(ctx);
    const serverRid = zlink.RoutingId.from(Buffer.from('control-server'));
    const clientRid = zlink.RoutingId.from(Buffer.from('control-client'));
    server.setRoutingId(serverRid);
    client.setRoutingId(clientRid);
    client.options.setConnectRoutingId(serverRid);
    let delivered = null;
    let replacedHandlerCalled = false;
    server.setCompletionControlHandler(() => {
        replacedHandlerCalled = true;
    });
    server.setCompletionControlHandler((sourceRoutingId, parts) => {
        delivered = {
            source: sourceRoutingId.toBytes().toString(),
            values: parts.map((part) => part.data().toString())
        };
        for (const part of parts)
            part.close();
    });
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    poller.add(server, [zlink.PollEventFlag.PollCompletion], 1);
    server.bind('inproc://node-completion-control');
    client.connect('inproc://node-completion-control');
    client.send(serverRid).message('application-unread').submit();
    const first = zlink.Message.from('opaque-command');
    const second = zlink.Message.from('generation-1');
    assert.equal(client.trySendCompletionControl(serverRid, [first, second]), true);
    assert.equal(first.data().toString(), 'opaque-command');
    assert.equal(second.data().toString(), 'generation-1');
    assert.equal(poller.wait(events, 2000), 1);
    await new Promise((resolve) => setImmediate(resolve));
    assert.deepEqual(delivered, {
        source: 'control-client',
        values: ['opaque-command', 'generation-1']
    });
    assert.equal(replacedHandlerCalled, false);
    const application = new zlink.Received();
    assert.equal(server.recv(application), true);
    assert.equal(application.parts[0].data().toString(), 'application-unread');
    first.close();
    second.close();
    application.close();
    poller.remove(server);
    events.close();
    poller.close();
    client.close();
    server.close();
    ctx.close();
});
test('completion control handlers are not limited to eight sockets or replacements', async () => {
    const ctx = zlink.createContext();
    const routers = Array.from({ length: 12 }, () => zlink.createRouterSocket(ctx));
    for (const router of routers) {
        for (let replacement = 0; replacement < 12; replacement += 1) {
            router.setCompletionControlHandler(() => { });
        }
    }
    for (const router of routers)
        router.close();
    ctx.close();
    await new Promise((resolve) => setImmediate(resolve));
});
test('socket close drains completion control payloads already accepted by Core', async () => {
    const ctx = zlink.createContext();
    const server = zlink.createRouterSocket(ctx);
    const client = zlink.createRouterSocket(ctx);
    const serverRid = zlink.RoutingId.from(Buffer.from('close-drain-server'));
    const clientRid = zlink.RoutingId.from(Buffer.from('close-drain-client'));
    server.setRoutingId(serverRid);
    client.setRoutingId(clientRid);
    client.options.setConnectRoutingId(serverRid);
    let delivered = false;
    server.setCompletionControlHandler((_sourceRoutingId, parts) => {
        delivered = true;
        for (const part of parts)
            part.close();
    });
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    poller.add(server, [zlink.PollEventFlag.PollCompletion], 1);
    server.bind('inproc://node-completion-control-close-drain');
    client.connect('inproc://node-completion-control-close-drain');
    const payload = zlink.Message.from('accepted-before-close');
    assert.equal(client.trySendCompletionControl(serverRid, [payload]), true);
    assert.equal(poller.wait(events, 2000), 1);
    poller.remove(server);
    server.close();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(delivered, true);
    payload.close();
    events.close();
    poller.close();
    client.close();
    ctx.close();
});
