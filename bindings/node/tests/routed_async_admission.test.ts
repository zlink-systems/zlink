// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

let endpointSequence = 0;

function endpoint(label: string): string {
  endpointSequence += 1;
  return `inproc://node-send-completion-${label}-${process.pid}-${endpointSequence}`;
}

function nextTurn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function within<T>(promise: Promise<T>, timeoutMs = 1_000): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`operation did not settle within ${timeoutMs}ms`)),
      timeoutMs
    );
    promise.then(
      (value) => { clearTimeout(timer); resolve(value); },
      (error) => { clearTimeout(timer); reject(error); }
    );
  });
}

function closeAll(context: any, ...sockets: any[]): void {
  for (const socket of sockets.reverse()) {
    try { socket.close(); } catch { /* cleanup should not mask the assertion */ }
  }
  context.close();
}

test('managed PAIR send resolves from Core completion and consumes at submit', async () => {
  const context = zlink.createContext();
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  sender.bind(endpoint('inline'));
  receiver.connect(sender.options.lastEndpoint);

  const payload = zlink.Message.from('completion');
  try {
    const result = sender.send().message(payload).submit();
    assert.equal(typeof result.then, 'function');
    await result;
    assert.equal(payload.size(), 0);
    const received = new zlink.Received();
    assert.equal(receiver.recv(received), true);
    assert.equal(received.singlePartOrThrow().getString(), 'completion');
    received.close();
  } finally {
    closeAll(context, sender, receiver);
    payload.close();
  }
});

test('Core timeout maps to per-operation SubmitError with the Core errno', async () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  sender.options.sendHwm = 4_096n;
  receiver.options.recvHwm = 4_096n;
  sender.bind(endpoint('timeout'));
  receiver.connect(sender.options.lastEndpoint);

  // Multipart routed records remain one-part here until the parallel Core
  // multipart ROUTER/DEALER defect fix lands.
  const first = zlink.Message.from(Buffer.alloc(4_096, 0x61));
  const pending = zlink.Message.from(Buffer.alloc(4_096, 0x62));
  try {
    await sender.send().message(first).submit();
    await assert.rejects(
      within(sender.send().message(pending).timeout(20).submit()),
      (error: unknown) => error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.Backpressured
        && (error as { nativeErrno: number }).nativeErrno !== 0
    );
    // Ownership transfers when zlink_send_async returns OK, even when the
    // eventual Core completion is TIMED_OUT.
    assert.equal(pending.size(), 0);
  } finally {
    closeAll(context, sender, receiver);
    first.close();
    pending.close();
  }
});

test('Core terminal completion rejects a pending send without a binding cancel API', async () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  sender.options.sendHwm = 4_096n;
  receiver.options.recvHwm = 4_096n;
  sender.bind(endpoint('terminal'));
  receiver.connect(sender.options.lastEndpoint);

  const first = zlink.Message.from(Buffer.alloc(4_096, 0x63));
  const pending = zlink.Message.from(Buffer.alloc(4_096, 0x64));
  try {
    await sender.send().message(first).submit();
    const result = sender.send().message(pending).timeout(-1).submit();
    await nextTurn();
    sender.close();
    await assert.rejects(
      within(result),
      (error: unknown) => error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.Terminated
        && (error as { nativeErrno: number }).nativeErrno !== 0
    );
    assert.equal(pending.size(), 0);
  } finally {
    closeAll(context, sender, receiver);
    first.close();
    pending.close();
  }
});

test('request Promise is settled only by the Core reply callback', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  dealer.setRoutingId(zlink.RoutingId.from('request-client'));
  router.bind(endpoint('request'));
  dealer.connect(router.options.lastEndpoint);

  try {
    const replyPromise = dealer.request().message('request').timeout(1_000).submit();
    let request: any | null = null;
    for (let attempt = 0; attempt < 100 && !request; attempt += 1) {
      const candidate = new zlink.Received();
      if (router.recv(candidate, zlink.RecvFlags.DontWait)) request = candidate;
      else candidate.close();
      if (!request) await nextTurn();
    }
    assert.ok(request);
    request.reply().message('reply').submit();
    const parts = await within(replyPromise) as any[];
    assert.equal(parts.length, 1);
    assert.equal(parts[0].getString(), 'reply');
    parts[0].close();
    request.close();
  } finally {
    closeAll(context, dealer, router);
  }
});
