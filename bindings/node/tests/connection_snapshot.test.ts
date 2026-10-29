// SPDX-License-Identifier: MPL-2.0
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

function waitConnection(socket: any, poller: any, events: any, state: number): any {
  const deadline = Date.now() + 3000;
  const baseline = socket.connectionAttemptsSnapshot()
    .find((value: any) => value.state === state);
  if (baseline) return baseline;
  while (Date.now() < deadline) {
    const count = poller.wait(events, Math.max(0, deadline - Date.now()));
    for (let i = 0; i < count; ++i) {
      if (!events.hasEvent(i, zlink.PollEventFlag.PollConnect)) continue;
      const row = socket.connectionAttemptsSnapshot()
        .find((value: any) => value.state === state);
      if (row) return row;
    }
  }
  assert.fail(`connection state ${state} was not observed`);
}

test('connection snapshot exists on every connectable family and reports closed query errors', () => {
  const ctx = zlink.createContext();
  try {
    for (const create of [zlink.createPairSocket, zlink.createPubSocket,
      zlink.createSubSocket, zlink.createXPubSocket, zlink.createXSubSocket,
      zlink.createDealerSocket, zlink.createRouterSocket]) {
      const socket = create(ctx);
      assert.deepEqual(socket.connectionAttemptsSnapshot(), []);
      socket.close();
      assert.throws(() => socket.connectionAttemptsSnapshot(),
        (error: any) => error instanceof zlink.ConfigError
          && error.result === zlink.ConfigResult.InvalidHandle);
    }
    const stream = zlink.createStreamSocket(ctx);
    assert.equal(stream.connectionAttemptsSnapshot, undefined);
    stream.close();
  } finally {
    ctx.close();
  }
});

test('PollConnect remains ready until a full snapshot; route readiness is independent', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(2);
  try {
    assert.equal(zlink.PollEventFlag.PollConnect, 128);
    router.bind('tcp://127.0.0.1:*');
    const endpoint = router.options.lastEndpoint;
    poller.add(dealer, [zlink.PollEventFlag.PollConnect], 10);
    poller.add(router, [zlink.PollEventFlag.PollRoute], 20);
    dealer.connect(endpoint);
    const deadline = Date.now() + 3000;
    let connectReady = false;
    let routeReady = false;
    while (Date.now() < deadline && !(connectReady && routeReady)) {
      const count = poller.wait(events, Math.max(0, deadline - Date.now()));
      for (let i = 0; i < count; ++i) {
        if (events.slot(i) === 10) connectReady ||= events.hasEvent(i, zlink.PollEventFlag.PollConnect);
        if (events.slot(i) === 20) routeReady ||= events.hasEvent(i, zlink.PollEventFlag.PollRoute);
      }
    }
    assert.equal(connectReady, true);
    assert.equal(routeReady, true);
    // Consuming the route snapshot does not consume connection readiness.
    assert.equal(router.routesSnapshot().length, 1);
    assert.equal(poller.wait(events, 0), 1);
    assert.equal(events.slot(0), 10);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollConnect), true);
    const row = waitConnection(dealer, poller, events, zlink.ConnectionAttemptState.Connected);
    assert.equal(row.endpoint, endpoint);
    assert.equal(typeof row.intentId, 'bigint');
    assert.equal(typeof row.revision, 'bigint');
    assert.equal(row.lastFailureId, 0n);
    assert.equal(row.lastFailure.kind, zlink.ConnectionFailureKind.None);
    assert.equal(row.lastFailure.stage, zlink.ConnectionFailureStage.None);
    assert.equal(poller.wait(events, 0), 0);
    dealer.disconnect(endpoint);
    assert.deepEqual(dealer.connectionAttemptsSnapshot(), []);
    // Earlier endpoint strings remain owned after a later native snapshot.
    assert.equal(row.endpoint, endpoint);
  } finally {
    events.close();
    poller.close();
    dealer.close();
    router.close();
    ctx.close();
  }
});

test('snapshot retries both row and UTF-8 endpoint arenas without returning partial rows', () => {
  const ctx = zlink.createContext();
  const dealer = zlink.createDealerSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  try {
    poller.add(dealer, [zlink.PollEventFlag.PollConnect], 0);
    const endpoints = Array.from({ length: 12 }, (_, i) => `inproc://connection-${i}-${'x'.repeat(80)}`);
    for (const endpoint of endpoints) dealer.connect(endpoint);
    assert.equal(poller.wait(events, 0), 1);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollConnect), true);
    const rows = dealer.connectionAttemptsSnapshot();
    assert.equal(poller.wait(events, 0), 0);
    assert.equal(rows.length, endpoints.length);
    assert.deepEqual(rows.map((row: any) => row.endpoint).sort(), [...endpoints].sort());
    for (const row of rows) {
      assert.equal(typeof row.intentId, 'bigint');
      assert.equal(typeof row.revision, 'bigint');
      assert.equal(typeof row.lastFailureId, 'bigint');
    }
  } finally {
    events.close();
    poller.close();
    dealer.close();
    ctx.close();
  }
});

test('a failed IPC attempt is successful snapshot data with Core typed failure', () => {
  const ctx = zlink.createContext();
  const dealer = zlink.createDealerSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  try {
    poller.add(dealer, [zlink.PollEventFlag.PollConnect], 0);
    dealer.connect(`ipc:///tmp/zlink-node-missing-${process.pid}-${Date.now()}/socket`);
    const row = waitConnection(dealer, poller, events, zlink.ConnectionAttemptState.AttemptFailed);
    assert.equal(row.lastFailure.kind, zlink.ConnectionFailureKind.EndpointMissing);
    assert.equal(row.lastFailure.stage, zlink.ConnectionFailureStage.Transport);
    assert.notEqual(row.lastFailureId, 0n);
  } finally {
    events.close();
    poller.close();
    dealer.close();
    ctx.close();
  }
});

test('same socket preserves PollConnect and queued request completions independently', async () => {
  const path = require('node:path');
  const nativeModule = require(path.resolve(__dirname, '../../dist/zlink/runtime/native/native.js'));
  const originalRequireNative = nativeModule.requireNative;
  const native = originalRequireNative();
  const ctx = zlink.createContext();
  const dealer = zlink.createDealerSocket(ctx);
  const router = zlink.createRouterSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  const both = zlink.PollEventFlag.PollConnect | zlink.PollEventFlag.PollCompletion;
  try {
    router.bind(`inproc://snapshot-completion-${process.pid}`);
    poller.add(dealer, [zlink.PollEventFlag.PollConnect, zlink.PollEventFlag.PollCompletion], 0);
    dealer.connect(router.options.lastEndpoint);
    dealer.send().message('ready').submit_sync();
    const readyMessage = new zlink.Received();
    assert.equal(router.recv(readyMessage), true);
    readyMessage.close();
    for (let round = 0; round < 2; ++round) {
      const pending = dealer.request().message(`request-${round}`).timeout(3000).submit();
      let delivered = 0;
      const reply = pending.reply.then((parts: any[]) => { delivered += 1; return parts; });
      // Observe cleanup rejection after an earlier assertion fails. The original
      // reply remains awaited below, so its failure still fails the test.
      void reply.catch(() => undefined);
      const request = new zlink.Received();
      try {
        assert.equal(router.recv(request), true);
        request.reply().message(`reply-${round}`).submit();
      } finally {
        request.close();
      }
      let observedQueued = false;
      const waitInto = (...args: any[]) => {
        const count = native.pollerWaitInto(...args);
        const flags = count === 0 ? 0 : native.pollEventsRevents(args[1], 0);
        if ((flags & both) === both) {
          observedQueued = true;
          if (round === 1) {
            // This is the SAME public wait's native poller and result buffer,
            // before RuntimePoller has drained the queued completion.
            assert.equal(dealer.connectionAttemptsSnapshot().length, 1);
            assert.equal(native.pollerWaitInto(args[0], args[1], args[2], 0), count);
            const remaining = native.pollEventsRevents(args[1], 0);
            assert.equal(remaining & zlink.PollEventFlag.PollConnect, 0);
            assert.equal(remaining & zlink.PollEventFlag.PollCompletion, zlink.PollEventFlag.PollCompletion);
          }
        }
        return count;
      };
      nativeModule.requireNative = () => new Proxy({}, {
        get(_target, property) {
          return property === 'pollerWaitInto' ? waitInto : native[property];
        }
      });
      const deadline = Date.now() + 3000;
      while (!observedQueued && Date.now() < deadline) {
        poller.wait(events, Math.max(0, deadline - Date.now()));
      }
      assert.equal(observedQueued, true, 'connection readiness and completion must both be queued before drain');
      assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollCompletion), true);
      const parts = await reply;
      assert.equal(parts[0].getString(), `reply-${round}`);
      for (const part of parts) part.close();
      assert.equal(delivered, 1);
      assert.equal(poller.wait(events, 0), round === 0 ? 1 : 0);
      if (round === 0) {
        assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollConnect), true);
        assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollCompletion), false);
      }
    }
  } finally {
    nativeModule.requireNative = originalRequireNative;
    events.close();
    poller.close();
    dealer.close();
    router.close();
    ctx.close();
  }
});

test('same ROUTER keeps PollRoute ready after consuming PollConnect', () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const outboundPeer = zlink.createDealerSocket(ctx);
  const inboundPeer = zlink.createDealerSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  try {
    router.bind(`inproc://snapshot-route-in-${process.pid}`);
    outboundPeer.bind(`inproc://snapshot-route-out-${process.pid}`);
    poller.add(router, [zlink.PollEventFlag.PollConnect, zlink.PollEventFlag.PollRoute], 0);
    router.connect(outboundPeer.options.lastEndpoint);
    inboundPeer.connect(router.options.lastEndpoint);
    const both = zlink.PollEventFlag.PollConnect | zlink.PollEventFlag.PollRoute;
    const deadline = Date.now() + 3000;
    let ready = false;
    while (Date.now() < deadline) {
      const count = poller.wait(events, Math.max(0, deadline - Date.now()));
      if (count === 1 && (events.revents(0) & both) === both) {
        ready = true;
        break;
      }
    }
    assert.equal(ready, true);
    assert.equal(router.connectionAttemptsSnapshot().length, 1);
    assert.equal(poller.wait(events, 0), 1);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollRoute), true);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollConnect), false);
    assert.notEqual(router.routesSnapshot().length, 0);
  } finally {
    events.close();
    poller.close();
    inboundPeer.close();
    outboundPeer.close();
    router.close();
    ctx.close();
  }
});
