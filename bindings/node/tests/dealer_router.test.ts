'use strict';

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

test('router forwards an unread received Message through native storage', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://dealer-router-native-forward');
  dealer.connect('inproc://dealer-router-native-forward');

  dealer.send().message(Buffer.alloc(4096, 0x61)).submit();
  const received = new zlink.Received();
  assert.equal(router.recv(received), true);
  assert.ok(received.routingId);

  // Do not call data() before submit: the received native frame is the send
  // source. This is the same receive-to-send operation used by routed echo.
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

test('router nonblocking recv preserves order and multipart ownership across native batches', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  router.bind('inproc://dealer-router-native-recv-batch');
  dealer.connect('inproc://dealer-router-native-recv-batch');

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

test('router pending batch remains receivable after the native poll state is drained', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(3);
  const pairReceiver = zlink.createPairSocket(ctx);
  const pairSender = zlink.createPairSocket(ctx);

  router.bind('inproc://dealer-router-pending-batch-readiness');
  dealer.connect('inproc://dealer-router-pending-batch-readiness');
  pairReceiver.bind('inproc://dealer-router-pending-batch-other-source');
  pairSender.connect('inproc://dealer-router-pending-batch-other-source');
  poller.add(router, [zlink.PollEventFlag.PollIn], 0);
  poller.add(pairReceiver, [zlink.PollEventFlag.PollIn], 0);

  for (let index = 0; index < 16; index += 1) {
    dealer.send().message(`message-${index}`).submit();
  }
  assert.equal(poller.wait(events, 1000), 1);

  const first = new zlink.Received();
  assert.equal(router.recv(first, zlink.RecvFlags.DontWait), true);
  assert.equal(first.singlePartOrThrow().getString('utf8'), 'message-0');
  first.close();

  for (let index = 16; index < 20; index += 1) {
    dealer.send().message(`message-${index}`).submit();
  }
  pairSender.send().message('other-source').submit();
  assert.equal(poller.wait(events, 1000), 2);
  assert.deepEqual(
    [events.slot(0), events.slot(1)].sort((left, right) => left - right),
    [0, 0]
  );
  const other = new zlink.Received();
  assert.equal(pairReceiver.recv(other, zlink.RecvFlags.DontWait), true);
  other.close();

  for (let index = 1; index < 20; index += 1) {
    assert.equal(poller.wait(events, 1000), 1, `missing readiness at message ${index}`);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollIn), true);
    assert.equal(events.slot(0), 0);
    const received = new zlink.Received();
    assert.equal(router.recv(received, zlink.RecvFlags.DontWait), true);
    assert.equal(received.singlePartOrThrow().getString('utf8'), `message-${index}`);
    received.close();
  }

  const empty = new zlink.Received();
  assert.equal(router.recv(empty, zlink.RecvFlags.DontWait), false);
  empty.close();
  events.close();
  poller.close();
  pairSender.close();
  pairReceiver.close();
  dealer.close();
  router.close();
  ctx.close();
});
