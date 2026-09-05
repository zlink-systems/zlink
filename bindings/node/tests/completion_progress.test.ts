// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { AsyncLocalStorage } from 'node:async_hooks';

const zlink = require('@zlink-systems/zlink');

test('runtime completion resolves consecutive requests without another event-loop source', () => {
  const packagePath = path.resolve(__dirname, '../../dist');
  const child = spawnSync(process.execPath, ['-e', `
    const assert = require('node:assert/strict');
    const z = require(${JSON.stringify(packagePath)});
    (async () => {
      const ctx = z.createContext();
      const router = z.createRouterSocket(ctx);
      const dealer = z.createDealerSocket(ctx);
      const received = new z.Received();
      router.bind('inproc://completion-progress');
      dealer.connect('inproc://completion-progress');
      try {
        for (let index = 0; index < 10; ++index) {
          const pending = dealer.request().message(String(index)).timeout(1000).submit();
          assert.equal(router.recv(received), true);
          received.reply().message(String(index)).submit();
          received.close();
          const parts = await pending;
          assert.equal(parts[0].getString(), String(index));
          parts.forEach(part => part.close());
        }
        process.stdout.write('completed');
      } finally {
        received.close(); dealer.close(); router.close(); ctx.close();
      }
    })().catch(error => { console.error(error); process.exitCode = 1; });
  `], { encoding: 'utf8', timeout: 5000 });
  assert.equal(child.error, undefined, child.error?.message);
  assert.equal(child.status, 0, child.stderr);
  assert.equal(child.stdout, 'completed', child.stderr);
});

test('pending requests move between runtime and public completion ownership without consuming DATA', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const received = new zlink.Received();
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  router.bind('inproc://completion-owner-transfer');
  dealer.connect('inproc://completion-owner-transfer');
  try {
    const first = dealer.request().message('public').timeout(1000).submit();
    assert.equal(router.recv(received), true);
    poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 31);
    received.reply().message('first').submit();
    received.close();
    assert.equal(poller.wait(events, 1000), 1);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollCompletion), true);
    const firstParts = await first;
    assert.equal(firstParts[0].getString(), 'first');
    firstParts.forEach(part => part.close());

    const second = dealer.request().message('runtime').timeout(1000).submit();
    assert.equal(router.recv(received), true);
    const peer = received.routingId;
    received.reply().message('second').submit();
    received.close();
    router.send(peer).message('application-data').submit_sync();
    assert.equal(poller.remove(dealer), true);
    const secondParts = await second;
    assert.equal(secondParts[0].getString(), 'second');
    secondParts.forEach(part => part.close());
    assert.equal(dealer.recv(received), true);
    assert.equal(received.parts[0].getString(), 'application-data');
  } finally {
    events.close(); poller.close(); received.close();
    dealer.close(); router.close(); ctx.close();
  }
});

test('runtime completion survives shutdown of an independent Context', async () => {
  const groups = Array.from({ length: 2 }, (_, group) => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const address = `inproc://completion-context-${group}`;
    router.bind(address);
    const dealers = Array.from({ length: 32 }, () => {
      const socket = zlink.createDealerSocket(ctx);
      socket.connect(address);
      return socket;
    });
    return { ctx, router, dealers };
  });
  const exchange = async (group) => {
    const pending = group.dealers.map((socket, index) =>
      socket.request().message(String(index)).timeout(1000).submit());
    const received = new zlink.Received();
    try {
      for (let index = 0; index < pending.length; ++index) {
        assert.equal(group.router.recv(received), true);
        const value = received.parts[0].getString();
        received.reply().message(value).submit();
        received.close();
      }
      const results = await Promise.all(pending);
      results.forEach((parts, index) => {
        assert.equal(parts[0].getString(), String(index));
        parts.forEach(part => part.close());
      });
    } finally { received.close(); }
  };
  const close = (group) => {
    group.dealers.forEach(socket => socket.close());
    group.router.close(); group.ctx.close();
  };
  try {
    await Promise.all(groups.map(exchange));
    const terminated = groups[0].dealers[0].request()
      .message('shutdown').timeout(1000).submit();
    const rejected = assert.rejects(terminated, (error: any) =>
      error instanceof zlink.RequestError && error.result === zlink.RequestResult.Terminated);
    groups[0].ctx.shutdown();
    await rejected;
    await exchange(groups[1]);
    close(groups[0]);
    await exchange(groups[1]);
  } finally {
    groups.forEach(close);
  }
});

test('native completion callbacks retain async context and run Promise continuations', async () => {
  const native = require(path.resolve(__dirname, '../../build/Release/zlink.node'));
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const received = new zlink.Received();
  const scope = new AsyncLocalStorage<string>();
  let watch: unknown = null;
  router.bind('inproc://completion-callback-scope');
  dealer.connect('inproc://completion-callback-scope');
  try {
    const submitted = native.socketSubmitRequest(
      dealer._native, null, Buffer.from('callback'), 1000, zlink.SendFlags.DontWait, 97n);
    assert.equal(submitted.result, zlink.SubmitResult.Ok);
    const finished = new Promise<void>((resolve, reject) => {
      watch = scope.run('request-owner', () => native.socketReadableWatchStart(dealer._native, (status: number) => {
        try {
          assert.equal(scope.getStore(), 'request-owner');
          assert.equal(status, 0);
          const completion = native.socketCompletionRecv(dealer._native, zlink.RecvFlags.DontWait);
          if (!completion) return;
          assert.equal(completion.requestResult, zlink.RequestResult.Ok);
          native.socketReadableWatchStop(watch);
          watch = null;
          const order: string[] = [];
          void Promise.resolve().then(() => { order.push('promise'); });
          setImmediate(() => {
            try {
              order.push('immediate');
              assert.deepEqual(order, ['promise', 'immediate']);
              resolve();
            } catch (error) { reject(error); }
          });
        } catch (error) { reject(error); }
      }));
    });
    assert.equal(router.recv(received), true);
    received.reply().message('reply').submit();
    received.close();
    await finished;
  } finally {
    if (watch !== null) native.socketReadableWatchStop(watch);
    received.close(); dealer.close(); router.close(); ctx.close();
  }
});

test('public completion ownership defers settlement until wait and close rejects runtime requests', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const received = new zlink.Received();
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  router.bind('inproc://completion-explicit-owner');
  dealer.connect('inproc://completion-explicit-owner');
  try {
    const pending = dealer.request().message('owned').timeout(1000).submit();
    let settled = false;
    void pending.then(() => { settled = true; });
    poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 7);
    assert.equal(router.recv(received), true);
    received.reply().message('reply').submit();
    received.close();
    await new Promise<void>(resolve => setImmediate(resolve));
    assert.equal(settled, false);
    assert.equal(poller.wait(events, 1000), 1);
    (await pending).forEach(part => part.close());
    poller.remove(dealer);
    const closing = dealer.request().message('closing').timeout(1000).submit();
    const rejected = assert.rejects(closing, (error: any) =>
      error instanceof zlink.RequestError && error.result === zlink.RequestResult.Terminated);
    dealer.close();
    await rejected;
  } finally {
    events.close(); poller.close(); received.close();
    dealer.close(); router.close(); ctx.close();
  }
});
