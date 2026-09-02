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
