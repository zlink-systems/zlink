'use strict';

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

test('router recv and reply still work through the canonical socket surface', async () => {
  const ctx = zlink.createContext();
  const routerSocket = zlink.createRouterSocket(ctx);
  const dealerSocket = zlink.createDealerSocket(ctx);
  const clientRoutingId = zlink.RoutingId.from(Buffer.from('request-reply-client'));

  routerSocket.bind('inproc://request-reply-contract');
  dealerSocket.setRoutingId(clientRoutingId);
  dealerSocket.connect('inproc://request-reply-contract');

  await dealerSocket.send().message('ping').submit();
  const request = new zlink.Received();
  routerSocket.recv(request);
  assert.ok(request.routingId instanceof zlink.RoutingId);
  assert.equal(request.routingId.toBytes().toString(), 'request-reply-client');
  assert.equal(request.requestSeq, null);
  assert.equal(request.parts[0].data().toString(), 'ping');
  await routerSocket.send(request.routingId).message('pong').submit();

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

  assert.throws(
    () => routerSocket.reply(routingId, 1n).message('pong').flags(zlink.SendFlags.DontWait).submit(),
    (error) => error instanceof zlink.SubmitError && error.result === zlink.SubmitResult.NotSupported
  );
  routerSocket.close();
  ctx.close();
});
