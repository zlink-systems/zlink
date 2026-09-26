'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

const WAIT_MS = 3_000;

function waitRoute(router: any, poller: any, events: any, peer: string, previous = 0n): bigint {
  const deadline = Date.now() + WAIT_MS;
  while (Date.now() < deadline) {
    const row = router.routesSnapshot().find((value: any) => value.routingId.toString() === peer);
    if (row !== undefined && row.routeGeneration !== previous) return row.routeGeneration;
    poller.wait(events, 10);
  }
  assert.fail(`selected route of ${peer} did not change from ${previous}`);
}

function routeReady(poller: any, events: any): boolean {
  const deadline = Date.now() + WAIT_MS;
  while (Date.now() < deadline) {
    const count = poller.wait(events, 10);
    for (let index = 0; index < count; index += 1) {
      if (events.hasEvent(index, zlink.PollEventFlag.PollRoute)) return true;
    }
  }
  return false;
}

test('ROUTER selected-route snapshot, PollRoute readiness and received route generation', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  router.options.handover = true;
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  let dealer = zlink.createDealerSocket(ctx);
  try {
    assert.equal(zlink.PollEventFlag.PollRoute, 64);
    router.bind('tcp://127.0.0.1:*');
    const endpoint = router.options.lastEndpoint;
    poller.add(router, [zlink.PollEventFlag.PollRoute], 0);
    assert.deepEqual(router.routesSnapshot(), []);

    dealer.setRoutingId(zlink.RoutingId.from('route-peer'));
    dealer.connect(endpoint);
    assert.equal(routeReady(poller, events), true);
    const first = waitRoute(router, poller, events, 'route-peer');
    assert.notEqual(first, 0n);

    dealer.send().message('first').submit_sync();
    const received = new zlink.Received();
    assert.equal(router.recv(received), true);
    assert.equal(received.parts[0].getString(), 'first');
    assert.equal(received.routeGeneration, first);
    received.close();

    // A reconnect of the same routing id replaces the selected route.
    dealer.close();
    dealer = zlink.createDealerSocket(ctx);
    dealer.setRoutingId(zlink.RoutingId.from('route-peer'));
    dealer.connect(endpoint);
    const second = waitRoute(router, poller, events, 'route-peer', first);
    assert.notEqual(second, 0n);
    assert.notEqual(second, first);

    dealer.send().message('second').submit_sync();
    assert.equal(router.recv(received), true);
    assert.equal(received.parts[0].getString(), 'second');
    assert.equal(received.routeGeneration, second);
    received.close();
  } finally {
    dealer.close();
    events.close();
    poller.close();
    router.close();
    ctx.close();
  }
});

test('ROUTER selected-route snapshot returns every route beyond the initial capacity', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  const dealers: any[] = [];
  try {
    router.bind('tcp://127.0.0.1:*');
    const endpoint = router.options.lastEndpoint;
    poller.add(router, [zlink.PollEventFlag.PollRoute], 0);
    const names = Array.from({ length: 12 }, (_, index) => `route-many-${index}`);
    for (const name of names) {
      const dealer = zlink.createDealerSocket(ctx);
      dealers.push(dealer);
      dealer.setRoutingId(zlink.RoutingId.from(name));
      dealer.connect(endpoint);
    }
    for (const name of names) waitRoute(router, poller, events, name);
    const rows = router.routesSnapshot();
    assert.deepEqual(
      rows.map((row: any) => row.routingId.toString()).sort(),
      [...names].sort()
    );
    for (const row of rows) assert.notEqual(row.routeGeneration, 0n);
  } finally {
    for (const dealer of dealers) dealer.close();
    events.close();
    poller.close();
    router.close();
    ctx.close();
  }
});
