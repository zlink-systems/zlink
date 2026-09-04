// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { Worker } = require('node:worker_threads');
const zlink = require('@zlink-systems/zlink');

let sequence = 0;
function endpoint(label: string): string {
  return `inproc://node-pull-completion-${label}-${process.pid}-${++sequence}`;
}

function closeAll(context: any, ...items: any[]): void {
  for (const item of items.reverse()) {
    try { item.close(); } catch { /* preserve the assertion */ }
  }
  context.close();
}

async function flushImmediateTurns(count = 64): Promise<void> {
  for (let turn = 0; turn < count; turn += 1) {
    await new Promise<void>((resolve) => setImmediate(resolve));
  }
}

test('send submit is Promise-based while submit_sync is flag-free', async () => {
  const context = zlink.createContext();
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  sender.bind(endpoint('send'));
  receiver.connect(sender.options.lastEndpoint);
  try {
    const managed = zlink.Message.from('managed');
    await sender.send().message(managed).submit();
    assert.equal(managed.size(), 0);
    sender.send().message('blocking').submit_sync();
    const first = new zlink.Received();
    const second = new zlink.Received();
    assert.equal(receiver.recv(first), true);
    assert.equal(receiver.recv(second), true);
    assert.deepEqual([first.parts[0].getString(), second.parts[0].getString()],
      ['managed', 'blocking']);
    first.close(); second.close(); managed.close();
  } finally { closeAll(context, sender, receiver); }
});

test('routed send without a route fails immediately and preserves Message ownership', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const payload = zlink.Message.from('no-route');
  router.options.mandatory = true;
  try {
    await assert.rejects(
      router.send(zlink.RoutingId.from('missing-peer')).message(payload).submit(),
      (error: unknown) => error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.NotConnected
    );
    assert.equal(payload.getString(), 'no-route');
  } finally {
    payload.close();
    closeAll(context, router);
  }
});

test('request Promise settles from a pulled completion', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  router.bind(endpoint('request'));
  dealer.connect(router.options.lastEndpoint);
  try {
    const pending = dealer.request().message('question').timeout(1_000).submit();
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    assert.ok(request.replyToken instanceof zlink.ReplyToken);
    request.reply().message('answer').submit();
    const reply = await pending;
    assert.equal(reply[0].getString(), 'answer');
    reply[0].close(); request.close();
  } finally { closeAll(context, dealer, router); }
});

test('request submit_sync blocks in native and returns reply parts', async () => {
  const worker = new Worker(`
    const { parentPort, workerData } = require('node:worker_threads');
    const z = require(workerData.modulePath);
    const ctx = z.createContext(); const router = z.createRouterSocket(ctx);
    router.bind('tcp://127.0.0.1:*'); parentPort.postMessage(router.options.lastEndpoint);
    const poll = () => {
      const request = new z.Received();
      if (router.recv(request, z.RecvFlags.DontWait)) {
        request.reply().message('sync-answer').submit(); request.close();
        router.close(); ctx.close(); return;
      }
      request.close(); setImmediate(poll);
    }; poll();
  `, { eval: true, workerData: { modulePath: require.resolve('@zlink-systems/zlink') } });
  const remote = await new Promise<string>((resolve, reject) => {
    worker.once('message', resolve); worker.once('error', reject);
  });
  const context = zlink.createContext();
  const dealer = zlink.createDealerSocket(context);
  dealer.connect(remote);
  try {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50);
    const reply = dealer.request().message('sync-question').timeout(2_000).submit_sync();
    assert.equal(reply[0].getString(), 'sync-answer');
    reply[0].close();
  } finally {
    closeAll(context, dealer);
    await worker.terminate();
  }
});

test('public Poller owns completion draining and reports PollCompletion', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  router.bind(endpoint('poll-owner'));
  dealer.connect(router.options.lastEndpoint);
  poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 73);
  try {
    const pending = dealer.request().message('poll-question').timeout(1_000).submit();
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    request.reply().message('poll-answer').submit();
    assert.equal(poller.wait(events, 1_000), 1);
    assert.equal(events.slot(0), 73);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollCompletion), true);
    const reply = await pending;
    assert.equal(reply[0].getString(), 'poll-answer');
    reply[0].close(); request.close();
    assert.equal(poller.remove(dealer), true);
  } finally {
    events.close(); poller.close(); closeAll(context, dealer, router);
  }
});

test('PollOut-only observer cannot drain another Poller completion owner', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const owner = zlink.createPoller();
  const observer = zlink.createPoller();
  const ownerEvents = zlink.createPollEvents(1);
  const observerEvents = zlink.createPollEvents(1);
  let settled = false;
  router.bind(endpoint('poll-owner-isolation'));
  dealer.connect(router.options.lastEndpoint);
  owner.add(dealer, [zlink.PollEventFlag.PollCompletion], 81);
  observer.add(dealer, [zlink.PollEventFlag.PollOut], 82);
  try {
    assert.equal(observer.wait(observerEvents, 0), 0,
      'ordinary connectivity must not masquerade as writable credit');
    const pending = dealer.request().message('owner-isolation').timeout(30_000)
      .submit().then((parts: any[]) => {
        settled = true;
        return parts;
      });
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    request.reply().message('owned-completion').submit();

    await flushImmediateTurns();
    assert.equal(settled, false,
      'the public completion owner must retain the queued completion');
    assert.equal(observer.wait(observerEvents, 0), 0,
      'a REQUEST completion must not masquerade as writable credit');
    await Promise.resolve();
    assert.equal(settled, false,
      'PollOut-only wait must not drain a completion owned by another Poller');

    assert.equal(owner.wait(ownerEvents, 1_000), 1);
    assert.equal(ownerEvents.slot(0), 81);
    assert.equal(ownerEvents.hasEvent(0, zlink.PollEventFlag.PollCompletion), true);
    const reply = await pending;
    assert.equal(reply[0].getString(), 'owned-completion');
    reply[0].close();
    request.close();
  } finally {
    observerEvents.close();
    ownerEvents.close();
    observer.close();
    owner.close();
    closeAll(context, dealer, router);
  }
});

test('duplicate socket add failure preserves the original completion owner', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  let settled = false;
  router.bind(endpoint('poll-duplicate-rollback'));
  dealer.connect(router.options.lastEndpoint);
  poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 83);
  try {
    assert.throws(
      () => poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 84),
      (error: unknown) => error instanceof zlink.ConfigError
    );
    assert.equal(poller.size, 1,
      'failed duplicate add must leave the original registration installed');

    const pending = dealer.request().message('duplicate-owner').timeout(30_000)
      .submit().then((parts: any[]) => {
        settled = true;
        return parts;
      });
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    request.reply().message('duplicate-owner-reply').submit();
    await flushImmediateTurns();
    assert.equal(settled, false,
      'duplicate-add rollback must not transfer ownership to the runtime pump');

    assert.equal(poller.wait(events, 1_000), 1);
    assert.equal(events.slot(0), 83);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollCompletion), true);
    const reply = await pending;
    assert.equal(reply[0].getString(), 'duplicate-owner-reply');
    reply[0].close();
    request.close();
  } finally {
    events.close();
    poller.close();
    closeAll(context, dealer, router);
  }
});

test('second PollCompletion owner is rejected without displacing the first', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const owner = zlink.createPoller();
  const contender = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  let settled = false;
  router.bind(endpoint('poll-owner-conflict'));
  dealer.connect(router.options.lastEndpoint);
  owner.add(dealer, [zlink.PollEventFlag.PollCompletion], 84);
  try {
    assert.throws(
      () => contender.add(dealer, [zlink.PollEventFlag.PollCompletion], 85),
      (error: unknown) => error instanceof zlink.ConfigError
        && (error as { result: number }).result === zlink.ConfigResult.InvalidState
    );

    const pending = dealer.request().message('owner-conflict').timeout(30_000)
      .submit().then((parts: any[]) => {
        settled = true;
        return parts;
      });
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    request.reply().message('first-owner-reply').submit();
    await flushImmediateTurns();
    assert.equal(settled, false,
      'failed ownership transfer must leave the first public owner active');

    assert.equal(owner.wait(events, 1_000), 1);
    assert.equal(events.slot(0), 84);
    const reply = await pending;
    assert.equal(reply[0].getString(), 'first-owner-reply');
    reply[0].close();
    request.close();
  } finally {
    events.close();
    contender.close();
    owner.close();
    closeAll(context, dealer, router);
  }
});

test('duplicate public slots drain only the socket named by the native event', async () => {
  const context = zlink.createContext();
  const routerA = zlink.createRouterSocket(context);
  const dealerA = zlink.createDealerSocket(context);
  const routerB = zlink.createRouterSocket(context);
  const dealerB = zlink.createDealerSocket(context);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  let settledA = false;
  let settledB = false;
  routerA.bind(endpoint('duplicate-slot-a'));
  dealerA.connect(routerA.options.lastEndpoint);
  routerB.bind(endpoint('duplicate-slot-b'));
  dealerB.connect(routerB.options.lastEndpoint);
  poller.add(dealerA, [zlink.PollEventFlag.PollCompletion], 85);
  poller.add(dealerB, [zlink.PollEventFlag.PollCompletion], 85);
  try {
    const pendingA = dealerA.request().message('question-a').timeout(30_000)
      .submit().then((parts: any[]) => {
        settledA = true;
        return parts;
      });
    const pendingB = dealerB.request().message('question-b').timeout(30_000)
      .submit().then((parts: any[]) => {
        settledB = true;
        return parts;
      });
    const requestA = new zlink.Received();
    const requestB = new zlink.Received();
    assert.equal(routerA.recv(requestA), true);
    assert.equal(routerB.recv(requestB), true);
    requestA.reply().message('answer-a').submit();
    requestB.reply().message('answer-b').submit();
    await flushImmediateTurns();

    assert.equal(poller.wait(events, 1_000), 1);
    assert.equal(events.slot(0), 85);
    await Promise.resolve();
    assert.equal(Number(settledA) + Number(settledB), 1,
      'one capacity-limited event must drain exactly one registered socket');

    assert.equal(poller.wait(events, 1_000), 1);
    assert.equal(events.slot(0), 85);
    const [replyA, replyB] = await Promise.all([pendingA, pendingB]);
    assert.equal(replyA[0].getString(), 'answer-a');
    assert.equal(replyB[0].getString(), 'answer-b');
    replyA[0].close();
    replyB[0].close();
    requestA.close();
    requestB.close();
  } finally {
    events.close();
    poller.close();
    closeAll(context, dealerB, routerB, dealerA, routerA);
  }
});

test('request non-OK completion rejects with typed RequestError only', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  router.bind(endpoint('timeout'));
  dealer.connect(router.options.lastEndpoint);
  try {
    const pending = dealer.request().message('never-replied').timeout(20).submit();
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    await assert.rejects(pending, (error: unknown) =>
      error instanceof zlink.RequestError
      && (error as { result: number }).result === zlink.RequestResult.TimedOut);
    request.close();
  } finally { closeAll(context, dealer, router); }
});

test('closing a socket rejects its live request with typed RequestError', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  router.bind(endpoint('request-close'));
  dealer.connect(router.options.lastEndpoint);
  try {
    const pending = dealer.request().message('close-before-reply').timeout(1_000).submit();
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    dealer.close();
    await assert.rejects(pending, (error: unknown) =>
      error instanceof zlink.RequestError
      && (error as { result: number }).result === zlink.RequestResult.Terminated);
    request.close();
  } finally { closeAll(context, dealer, router); }
});
