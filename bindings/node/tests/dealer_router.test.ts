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
