'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

async function waitForConnectionReady(
  monitor: { recv(flags?: number): { event: number } | null }
): Promise<void> {
  const deadline = Date.now() + 2_000;
  while (Date.now() < deadline) {
    const event = monitor.recv(zlink.RecvFlags.DontWait);
    if (event?.event === zlink.MonitorEventType.ConnectionReady) return;
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
  throw new Error('connection-ready monitor event timed out');
}

test('dealer/router uses routing id through Received and routed send', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://dealer-router-contract');
  dealer.connect('inproc://dealer-router-contract');

  await dealer.send().message('hello').submit();
  const request = new zlink.Received();
  router.recv(request);

  assert.equal(request.parts.length, 1);
  assert.ok(Object.isFrozen(request.parts));
  assert.equal(request.parts[0].data().toString(), 'hello');
  assert.ok(request.routingId instanceof zlink.RoutingId);
  assert.equal(request.parts[0].refCount(), 1);
  assert.notEqual(request.parts[0].getProperty('Routing-Id'), null);
  assert.equal(request.parts[0].getProperty('Routing-Id'), request.parts[0].getProperty('Identity'));

  await router.send(request.routingId).message('world').submit();

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

test('router recv fills caller-provided Received with routing metadata', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://dealer-router-recv-payload-into');
  dealer.connect('inproc://dealer-router-recv-payload-into');

  await dealer.send().message(Buffer.from('payload')).submit();

  const received = new zlink.Received();
  assert.equal(router.recv(received), true);
  assert.ok(received.routingId);
  assert.equal(received.requestSeq, null);
  assert.equal(received.singlePartOrThrow().data().toString(), 'payload');

  dealer.close();
  router.close();
  ctx.close();
});

test('dealer recv reuses Received from plain payload to replyable request', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const dealerRid = zlink.RoutingId.from(Buffer.from('dealer-recv-request'));
  const routerMonitor = router.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
  const dealerMonitor = dealer.monitorOpen([zlink.MonitorEventType.ConnectionReady]);

  try {
    router.bind('inproc://dealer-recv-request-metadata');
    dealer.setRoutingId(dealerRid);
    dealer.connect('inproc://dealer-recv-request-metadata');
    await waitForConnectionReady(routerMonitor);
    await waitForConnectionReady(dealerMonitor);
  } finally {
    dealerMonitor.close();
    routerMonitor.close();
  }

  const received = new zlink.Received();
  await router.send(dealerRid).message('plain').submit();
  assert.equal(dealer.recv(received), true);
  assert.equal(received.requestSeq, null);
  assert.equal(received.singlePartOrThrow().getString(), 'plain');
  assert.throws(() => received.reply(), zlink.SubmitError);

  const pendingReply = router.request(dealerRid)
    .message('request-head')
    .message('request-body')
    .timeout(2_000)
    .submit();
  assert.equal(dealer.recv(received), true);
  assert.ok(received.requestSeq !== null && received.requestSeq > 0n);
  assert.deepEqual(received.parts.map((part) => part.getString()), [
    'request-head',
    'request-body',
  ]);
  await received.reply().message('reply').submit();

  const keepAlive = setInterval(() => {}, 10);
  let replyParts: readonly { getString(): string; close(): void }[] | undefined;
  try {
    replyParts = await pendingReply;
    assert.equal(replyParts.length, 1);
    assert.equal(replyParts[0].getString(), 'reply');
  } finally {
    clearInterval(keepAlive);
    replyParts?.forEach((part) => part.close());
  }

  received.close();
  dealer.close();
  router.close();
  ctx.close();
});

test('router forwards an unread received Message from managed storage', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://dealer-router-managed-forward');
  dealer.connect('inproc://dealer-router-managed-forward');

  await dealer.send().message(Buffer.alloc(4096, 0x61)).submit();
  const received = new zlink.Received();
  assert.equal(router.recv(received), true);
  assert.ok(received.routingId);

  // Forward the JS-owned receive Buffer without reading it first. Submit
  // materializes the Core frame while preserving the public move contract.
  await router.send(received.routingId).message(received.singlePartOrThrow()).submit();
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

test('router repeatedly transfers native receive frames without stale state', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://dealer-router-native-frame-reuse');
  dealer.connect('inproc://dealer-router-native-frame-reuse');

  const received = new zlink.Received();
  const echoed = new zlink.Received();
  for (let index = 0; index < 256; index += 1) {
    const expected = `payload-${index}`;
    await dealer.send().message(expected).submit();
    assert.equal(router.recv(received), true);
    assert.ok(received.routingId);

    // Forward without data(): successful submit moves this exact native frame
    // to Core. The next recv may reuse its storage only after that ownership
    // transfer has completed and the public Message has been consumed.
    await router.send(received.routingId)
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

test('router nonblocking recv preserves order and async send ownership', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://dealer-router-recv-order');
  dealer.connect('inproc://dealer-router-recv-order');

  for (let index = 0; index < 80; index += 1) {
    // Multipart async generic-target behavior has a separate Core regression
    // gate. Keep this ordering probe one-part so it tests only receive order.
    await dealer.send()
      .message(Buffer.from(`payload-${index}`))
      .submit();
  }

  for (let index = 0; index < 80; index += 1) {
    const received = new zlink.Received();
    assert.equal(router.recv(received, zlink.RecvFlags.DontWait), true);
    assert.equal(received.parts.length, 1);
    assert.equal(received.parts[0].getString('utf8'), `payload-${index}`);
    received.close();
  }

  const empty = new zlink.Received();
  assert.equal(router.recv(empty, zlink.RecvFlags.DontWait), false);
  empty.close();
  dealer.close();
  router.close();
  ctx.close();
});
