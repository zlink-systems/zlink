// SPDX-License-Identifier: MPL-2.0
import test from 'node:test';
import assert from 'node:assert/strict';
const zlink = require('@zlink-systems/zlink');

test('multipart receive reuse preserves captured reply routes and collection identities', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealers = [zlink.createDealerSocket(ctx), zlink.createDealerSocket(ctx)];
  const received = new zlink.Received();
  const saved: any[] = [];
  const requests: Promise<any>[] = [];
  try {
    router.bind('inproc://routed-receive-ownership');
    for (let index = 0; index < dealers.length; ++index) {
      const dealer = dealers[index];
      dealer.setRoutingId(zlink.RoutingId.from(`peer-${index}`));
      dealer.connect('inproc://routed-receive-ownership');
      requests.push(dealer.request().message(`body-${index}`).message('').timeout(1000).submit());
      assert.equal(router.recv(received), true);
      const parts = received.parts;
      assert.ok(Object.isFrozen(parts));
      assert.equal(parts.length, 2);
      for (const part of parts) {
        assert.equal(part.getProperty('Routing-Id'), `peer-${index}`);
        assert.equal(part.getProperty('Identity'), `peer-${index}`);
      }
      saved.push({ parts, route: received.routingId, token: received.replyToken,
        reply: received.reply().message(`reply-${index}`), view: parts[0].data() });
    }
    assert.notStrictEqual(saved[0].parts, saved[1].parts);
    assert.equal(saved[0].parts.length, 2);
    assert.equal(saved[0].route.toBytes().toString(), 'peer-0');
    assert.equal(saved[0].view.toString(), 'body-0');
    assert.equal(saved[0].token.equals(saved[1].token), false);
    assert.equal(saved[0].token.equals(saved[0].token), true);
    assert.equal(saved[0].token.hashCode(), saved[0].token.hashCode());
    assert.throws(() => router.reply(saved[0].route, Object.create(zlink.ReplyToken.prototype)), TypeError);
    assert.throws(() => router.reply(saved[0].route, {}), TypeError);
    saved[1].reply.submit();
    saved[0].reply.submit();
    const replies = await Promise.all(requests);
    try { assert.deepEqual(replies.map(parts => parts[0].getString()), ['reply-0', 'reply-1']); }
    finally { for (const parts of replies) for (const part of parts) part.close(); }
  } finally {
    received.close();
    for (const dealer of dealers) dealer.close();
    router.close(); ctx.close();
  }
});
