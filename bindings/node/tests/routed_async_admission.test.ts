// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

let endpointSequence = 0;

function endpoint(label: string): string {
  endpointSequence += 1;
  return `inproc://node-routed-async-${label}-${process.pid}-${endpointSequence}`;
}

function nextTurn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function within<T>(promise: Promise<T>, timeoutMs = 2_000): Promise<T> {
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

async function registerDealer(
  dealer: any,
  router: any,
  label: string
): Promise<void> {
  await dealer.send().message(label).submit();
  const registration = new zlink.Received();
  assert.equal(router.recv(registration), true);
  registration.close();
}

async function receiveWithin(socket: any, timeoutMs = 2_000): Promise<any> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const received = new zlink.Received();
    if (socket.recv(received, zlink.RecvFlags.DontWait)) return received;
    received.close();
    if (Date.now() >= deadline) {
      throw new Error(`message did not arrive within ${timeoutMs}ms`);
    }
    await nextTurn();
  }
}

async function closeEventually(socket: any, timeoutMs = 2_000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    try {
      socket.close();
      return;
    } catch (error) {
      if (!(error instanceof zlink.CloseError)
          || (error as { result: number }).result !== zlink.CloseResult.Busy
          || Date.now() >= deadline) {
        throw error;
      }
      await nextTurn();
    }
  }
}

test('blocked routed submit keeps ownership and does not delay another target', async () => {
  const ctx = zlink.createContext();
  ctx.options.autoHwmEnabled = false;
  const router = zlink.createRouterSocket(ctx);
  const dealerA = zlink.createDealerSocket(ctx);
  const dealerB = zlink.createDealerSocket(ctx);
  const ridA = zlink.RoutingId.from('async-target-a');
  const ridB = zlink.RoutingId.from('async-target-b');
  dealerA.setRoutingId(ridA);
  dealerB.setRoutingId(ridB);
  router.options.sendHwm = 4_096n;
  router.options.sendTimeout = 2_000;
  dealerA.options.recvHwm = 4_096n;
  dealerB.options.recvHwm = 4_096n;

  router.bind(endpoint('isolation'));
  const address = router.options.lastEndpoint;
  dealerA.connect(address);
  dealerB.connect(address);

  try {
    await registerDealer(dealerA, router, 'register-a');
    await registerDealer(dealerB, router, 'register-b');

    const payload = Buffer.alloc(64 * 1_024, 0x61);
    await router.send(ridA).message(payload).submit();

    const pendingMessage = zlink.Message.from(payload);
    let pendingSettled = false;
    const pendingA = router.send(ridA).message(pendingMessage).submit();
    pendingA.then(
      () => { pendingSettled = true; },
      () => { pendingSettled = true; }
    );
    await nextTurn();
    assert.equal(pendingSettled, false);
    assert.equal(pendingMessage.size(), payload.length);

    await within(router.send(ridB).message(payload).submit());
    const receivedB = new zlink.Received();
    assert.equal(dealerB.recv(receivedB), true);
    assert.equal(receivedB.singlePartOrThrow().size(), payload.length);
    receivedB.close();

    const firstA = new zlink.Received();
    assert.equal(dealerA.recv(firstA), true);
    firstA.close();
    await within(pendingA);
    assert.equal(pendingMessage.size(), 0);

    const secondA = new zlink.Received();
    assert.equal(dealerA.recv(secondA), true);
    assert.equal(secondA.singlePartOrThrow().size(), payload.length);
    secondA.close();
    pendingMessage.close();
  } finally {
    await closeEventually(dealerB);
    await closeEventually(dealerA);
    await closeEventually(router);
    ctx.close();
  }
});

test('terminal wake fails only the detached exact target', async () => {
  const ctx = zlink.createContext();
  ctx.options.autoHwmEnabled = false;
  const router = zlink.createRouterSocket(ctx);
  const dealerA = zlink.createDealerSocket(ctx);
  const dealerB = zlink.createDealerSocket(ctx);
  const ridA = zlink.RoutingId.from('terminal-target-a');
  const ridB = zlink.RoutingId.from('terminal-target-b');
  dealerA.setRoutingId(ridA);
  dealerB.setRoutingId(ridB);
  router.options.sendHwm = 4_096n;
  router.options.sendTimeout = -1;
  dealerA.options.recvHwm = 4_096n;
  dealerB.options.recvHwm = 4_096n;

  router.bind(endpoint('terminal'));
  const address = router.options.lastEndpoint;
  dealerA.connect(address);
  dealerB.connect(address);

  try {
    await registerDealer(dealerA, router, 'register-a');
    await registerDealer(dealerB, router, 'register-b');

    const payload = Buffer.alloc(64 * 1_024, 0x62);
    await router.send(ridA).message(payload).submit();
    const pendingA = router.send(ridA).message(payload).submit();
    await nextTurn();

    await closeEventually(dealerA);
    await assert.rejects(
      within(pendingA),
      (error: unknown) => error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.NotConnected
    );

    await within(router.send(ridB).message('target-b-still-writable').submit());
    const receivedB = new zlink.Received();
    assert.equal(dealerB.recv(receivedB), true);
    assert.equal(
      receivedB.singlePartOrThrow().getString(),
      'target-b-still-writable'
    );
    receivedB.close();
  } finally {
    await closeEventually(dealerB);
    await closeEventually(dealerA);
    await closeEventually(router);
    ctx.close();
  }
});

test('request waits for exact HWM readiness and preserves fast-reply correlation', async () => {
  const ctx = zlink.createContext();
  ctx.options.autoHwmEnabled = false;
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const rid = zlink.RoutingId.from('request-hwm-target');
  dealer.setRoutingId(rid);
  dealer.options.sendHwm = 4_096n;
  router.options.recvHwm = 4_096n;
  const address = endpoint('request');
  router.bind(address);
  dealer.connect(address);

  try {
    await registerDealer(dealer, router, 'register-request-route');
    const payload = Buffer.alloc(64 * 1_024, 0x63);
    await dealer.send().message(payload).submit();

    let requestSettled = false;
    const reply: Promise<any[]> = dealer.request()
      .message('request-after-hwm')
      .timeout(2_000)
      .submit();
    reply.then(
      () => { requestSettled = true; },
      () => { requestSettled = true; }
    );
    await nextTurn();
    assert.equal(requestSettled, false);

    const occupying = new zlink.Received();
    assert.equal(router.recv(occupying), true);
    occupying.close();

    const request = await receiveWithin(router);
    assert.equal(request.singlePartOrThrow().getString(), 'request-after-hwm');
    assert.ok(request.requestSeq > 0n);
    request.reply().message('reply-after-hwm').submit();
    request.close();

    const replyParts = await within(reply);
    assert.equal(replyParts.length, 1);
    assert.equal(replyParts[0].getString(), 'reply-after-hwm');
    for (const part of replyParts) part.close();
  } finally {
    await closeEventually(dealer);
    await closeEventually(router);
    ctx.close();
  }
});

test('absolute timeout and socket close settle pending sends once without consuming input', async () => {
  const ctx = zlink.createContext();
  ctx.options.autoHwmEnabled = false;
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const rid = zlink.RoutingId.from('deadline-target');
  dealer.setRoutingId(rid);
  router.options.sendHwm = 4_096n;
  router.options.sendTimeout = 25;
  dealer.options.recvHwm = 4_096n;
  const address = endpoint('deadline');
  router.bind(address);
  dealer.connect(address);

  try {
    await registerDealer(dealer, router, 'register-deadline-route');
    const payload = Buffer.alloc(64 * 1_024, 0x64);
    await router.send(rid).message(payload).submit();

    const timedOutMessage = zlink.Message.from(payload);
    await assert.rejects(
      within(router.send(rid).message(timedOutMessage).submit(), 500),
      (error: unknown) => error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.Backpressured
    );
    assert.equal(timedOutMessage.size(), payload.length);

    router.options.sendTimeout = -1;
    const closedMessage = zlink.Message.from(payload);
    const pendingClose = router.send(rid).message(closedMessage).submit();
    await nextTurn();
    await closeEventually(router);
    await assert.rejects(
      within(pendingClose, 500),
      (error: unknown) => error instanceof zlink.SubmitError
        && (error as { result: number }).result === zlink.SubmitResult.Terminated
    );
    assert.equal(closedMessage.size(), payload.length);
    timedOutMessage.close();
    closedMessage.close();
  } finally {
    await closeEventually(dealer);
    await closeEventually(router);
    ctx.close();
  }
});
