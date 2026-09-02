'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

test('dealer/router send captures target and refills Received', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const dealerRid = zlink.RoutingId.from('dealer-router-client');
  router.bind('inproc://dealer-router-pull-completion');
  dealer.setRoutingId(dealerRid);
  dealer.connect('inproc://dealer-router-pull-completion');
  try {
    await dealer.send().message('hello').submit();
    const received = new zlink.Received();
    assert.equal(router.recv(received), true);
    assert.equal(received.parts[0].getString(), 'hello');
    assert.equal(received.replyToken, null);
    const captured = received.routingId;
    assert.ok(captured);
    const operation = router.send(captured).message('world');
    received.close();
    await operation.submit();
    assert.equal(dealer.recv(received), true);
    assert.equal(received.parts[0].getString(), 'world');
    received.close();
  } finally {
    dealer.close(); router.close(); ctx.close();
  }
});

test('request receive exposes opaque ReplyToken and reusable state resets', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  router.bind('inproc://dealer-router-reply-token');
  dealer.connect('inproc://dealer-router-reply-token');
  try {
    await dealer.send().message('plain').submit();
    const received = new zlink.Received();
    assert.equal(router.recv(received), true);
    assert.equal(received.replyToken, null);

    const pending = dealer.request().message('request').timeout(1_000).submit();
    assert.equal(router.recv(received), true);
    assert.ok(received.replyToken instanceof zlink.ReplyToken);
    assert.equal(received.replyToken.toString(), 'ReplyToken');
    received.reply().message('reply').submit();
    const reply = await pending;
    assert.equal(reply[0].getString(), 'reply');
    reply[0].close(); received.close();
  } finally {
    dealer.close(); router.close(); ctx.close();
  }
});

test('ReplyToken from another RouterSocket is rejected before native submit', async () => {
  const ctx = zlink.createContext();
  const first = zlink.createRouterSocket(ctx);
  const second = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  first.bind('inproc://reply-token-owner');
  dealer.connect('inproc://reply-token-owner');
  try {
    const pending = dealer.request().message('request').timeout(50).submit();
    const request = new zlink.Received();
    assert.equal(first.recv(request), true);
    assert.ok(request.routingId && request.replyToken);
    const message = zlink.Message.from('must-remain-owned');
    assert.throws(
      () => second.reply(request.routingId, request.replyToken).message(message).submit(),
      TypeError
    );
    assert.equal(message.getString(), 'must-remain-owned');
    message.close(); request.close();
    await assert.rejects(pending, zlink.RequestError);
  } finally {
    dealer.close(); second.close(); first.close(); ctx.close();
  }
});
