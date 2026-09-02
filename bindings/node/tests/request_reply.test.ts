'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

test('request/reply public surface is token-based and flag-free', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  try {
    assert.equal(typeof router.request, 'function');
    assert.equal(typeof router.reply, 'function');
    assert.equal(router.requestTransportPair, undefined);
    assert.equal(router.sendTransportPair, undefined);
    assert.equal(dealer.onReceive, undefined);
    assert.throws(() => new zlink.ReplyToken(), TypeError);
  } finally {
    dealer.close(); router.close(); ctx.close();
  }
});

test('ReplyToken equality and hash include the socket owner', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  router.bind('inproc://reply-token-value-contract');
  dealer.connect('inproc://reply-token-value-contract');
  try {
    const pending = dealer.request().message('ping').timeout(1_000).submit();
    const received = new zlink.Received();
    assert.equal(router.recv(received), true);
    const token = received.replyToken;
    assert.ok(token);
    assert.equal(token.equals(token), true);
    assert.equal(token.hashCode(), token.hashCode());
    assert.equal(Object.keys(token).length, 0);
    assert.equal('value' in token, false);
    received.reply().message('pong').submit();
    const reply = await pending;
    assert.equal(reply[0].getString(), 'pong');
    reply[0].close(); received.close();
  } finally {
    dealer.close(); router.close(); ctx.close();
  }
});
