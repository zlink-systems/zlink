// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

let sequence = 0;

function endpoint(): string {
  return `inproc://node-managed-send-backpressure-${process.pid}-${++sequence}`;
}

function yieldToEventLoop(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

function drainAvailable(socket: any, values: string[]): void {
  for (;;) {
    const received = new zlink.Received();
    try {
      if (!socket.recv(received, zlink.RecvFlags.DontWait)) return;
      values.push(received.singlePartOrThrow().getString());
    } finally {
      received.close();
    }
  }
}

test('managed send snapshots a backpressured Buffer and retries it after peer drain', async () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  const readiness = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  const payloads: Buffer[] = [];
  const sends: Promise<void>[] = [];
  const settledByIndex: boolean[] = [];
  const expected: string[] = [];
  const received: string[] = [];
  let settled = 0;
  let failure: unknown;

  try {
    sender.options.linger = 0;
    receiver.options.linger = 0;
    sender.options.immediate = true;
    sender.options.sendHwm = 512n;
    receiver.options.recvHwm = 512n;
    receiver.bind(endpoint());
    sender.connect(receiver.options.lastEndpoint);

    // Ordinary connectivity no longer publishes POLLOUT. A blocking prime is
    // the deterministic inproc attach synchronization point.
    sender.send().message('pair-ready').submit_sync();
    const prime = new zlink.Received();
    assert.equal(receiver.recv(prime), true);
    assert.equal(prime.singlePartOrThrow().getString(), 'pair-ready');
    prime.close();

    for (let index = 0; index < 64; index += 1) {
      const text = `${index.toString().padStart(4, '0')}:${'x'.repeat(59)}`;
      const payload = Buffer.from(text);
      expected.push(text);
      payloads.push(payload);
      settledByIndex.push(false);
      sends.push(sender.send().message(payload).submit().then(
        () => { settledByIndex[index] = true; settled += 1; },
        (error: unknown) => {
          settledByIndex[index] = true;
          settled += 1;
          failure ??= error;
        }
      ));
    }

    await yieldToEventLoop();
    assert.ok(settled > 0, 'at least one packet must be admitted before HWM');
    assert.ok(settled < sends.length, 'HWM must leave at least one send waiting');
    const pendingIndex = settledByIndex.findIndex((value) => !value);
    assert.notEqual(pendingIndex, -1);
    payloads[pendingIndex].fill(0x7a);

    // The first event-loop turn created the binding's lazy completion poller.
    // Transfer ownership to a public poller while sends are still waiting,
    // then prove that WRITABLE is surfaced as POLLOUT and processed there.
    readiness.add(sender, [
      zlink.PollEventFlag.PollOut,
      zlink.PollEventFlag.PollCompletion,
    ], 79);
    let sawWritablePollOut = false;
    for (let turn = 0; turn < sends.length && !sawWritablePollOut; turn += 1) {
      const next = new zlink.Received();
      try {
        if (receiver.recv(next, zlink.RecvFlags.DontWait)) {
          received.push(next.singlePartOrThrow().getString());
        }
      } finally {
        next.close();
      }
      if (readiness.wait(events, 0) > 0) {
        assert.equal(events.slot(0), 79);
        sawWritablePollOut = events.hasEvent(0, zlink.PollEventFlag.PollOut);
      }
      if (!sawWritablePollOut) await yieldToEventLoop();
    }
    assert.equal(sawWritablePollOut, true,
      'a matching WRITABLE completion must wake the public poller as POLLOUT');
    assert.equal(readiness.remove(sender), true);

    for (let turn = 0; turn < 10_000; turn += 1) {
      drainAvailable(receiver, received);
      if (settled === sends.length && received.length === sends.length) break;
      await yieldToEventLoop();
    }
    assert.equal(settled, sends.length,
      'every managed send must settle after the peer returns write credit');
    assert.equal(received.length, sends.length,
      'every logical packet must be delivered after managed retry');
    await Promise.all(sends);
    if (failure !== undefined) throw failure;

    assert.deepEqual(received, expected,
      'managed retry must send the packet snapshot captured at submit time');
  } finally {
    events.close();
    readiness.close();
    receiver.close();
    sender.close();
    context.close();
  }
});

test('retry completion does not consume a Message wrapper reused by the caller', async () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  const messages: any[] = [];
  const sends: Promise<void>[] = [];
  const expected: string[] = [];
  const received: string[] = [];
  let replacement: any = null;
  let settled = 0;
  let wrapperSettled = false;
  let failure: unknown;

  try {
    sender.options.linger = 0;
    receiver.options.linger = 0;
    sender.options.immediate = true;
    sender.options.sendHwm = 512n;
    receiver.options.recvHwm = 512n;
    receiver.bind(endpoint());
    sender.connect(receiver.options.lastEndpoint);
    sender.send().message('pair-ready').submit_sync();
    const prime = new zlink.Received();
    assert.equal(receiver.recv(prime), true);
    assert.equal(prime.singlePartOrThrow().getString(), 'pair-ready');
    prime.close();

    for (let index = 0; index < 64; index += 1) {
      const text = `${index.toString().padStart(4, '0')}:${'m'.repeat(59)}`;
      expected.push(text);
      sends.push(sender.send().message(Buffer.from(text)).submit().then(
        () => { settled += 1; },
        (error: unknown) => {
          settled += 1;
          failure ??= error;
        }
      ));
    }

    await yieldToEventLoop();
    assert.ok(settled > 0 && settled < sends.length,
      'Buffer sends must fill HWM before the wrapper regression attempt');

    const released = zlink.Message.from('wrapper-packet');
    messages.push(released);
    expected.push('wrapper-packet');
    sends.push(sender.send().message(released).submit().then(
      () => { wrapperSettled = true; settled += 1; },
      (error: unknown) => {
        wrapperSettled = true;
        settled += 1;
        failure ??= error;
      }
    ));
    assert.equal(released.size(), 0,
      'a backpressured submit must consume Message after taking its snapshot');
    assert.equal(wrapperSettled, false,
      'the full HWM must leave the wrapper packet awaiting WRITABLE');

    released.close();
    replacement = zlink.Message.from('replacement-wrapper');
    assert.strictEqual(replacement, released,
      'the caller should be able to reuse the consumed wrapper while retry waits');

    for (let turn = 0; turn < 10_000; turn += 1) {
      drainAvailable(receiver, received);
      if (settled === sends.length && received.length === sends.length) break;
      await yieldToEventLoop();
    }
    assert.equal(settled, sends.length);
    assert.equal(received.length, sends.length);
    await Promise.all(sends);
    if (failure !== undefined) throw failure;
    assert.deepEqual(received, expected);
    assert.equal(replacement.getString(), 'replacement-wrapper',
      'retry success must not consume the caller-reused wrapper');
  } finally {
    for (const message of new Set(messages)) message.close();
    replacement?.close();
    receiver.close();
    sender.close();
    context.close();
  }
});

test('context shutdown rejects a backpressured managed send as Terminated', async () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  const attempts: Array<{ settled: boolean; error?: unknown; done: Promise<void> }> = [];

  try {
    sender.options.linger = 0;
    receiver.options.linger = 0;
    sender.options.immediate = true;
    sender.options.sendHwm = 512n;
    receiver.options.recvHwm = 512n;
    receiver.bind(endpoint());
    sender.connect(receiver.options.lastEndpoint);
    sender.send().message('pair-ready').submit_sync();
    const prime = new zlink.Received();
    assert.equal(receiver.recv(prime), true);
    assert.equal(prime.singlePartOrThrow().getString(), 'pair-ready');
    prime.close();

    for (let index = 0; index < 64; index += 1) {
      const state: { settled: boolean; error?: unknown; done: Promise<void> } = {
        settled: false,
        done: Promise.resolve(),
      };
      state.done = sender.send().message(Buffer.alloc(64, index)).submit().then(
        () => { state.settled = true; },
        (error: unknown) => {
          state.error = error;
          state.settled = true;
        }
      );
      attempts.push(state);
    }

    await yieldToEventLoop();
    const pending = attempts.find((attempt) => !attempt.settled);
    assert.ok(pending, 'HWM must leave at least one managed send waiting');
    context.shutdown();
    for (let turn = 0; turn < 10_000 && !pending.settled; turn += 1) {
      await yieldToEventLoop();
    }
    assert.equal(pending.settled, true,
      'shutdown must terminate the pending WRITABLE wait');
    assert.ok(pending.error instanceof zlink.SubmitError);
    assert.equal((pending.error as { result: number }).result,
      zlink.SubmitResult.Terminated);
  } finally {
    receiver.close();
    sender.close();
    context.close();
    await Promise.all(attempts.map((attempt) => attempt.done));
  }
});

test('managed routed send retries the same target and packet after HWM drain', async () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const peer = zlink.RoutingId.from('node-managed-routed-peer');
  const payloads: Buffer[] = [];
  const sends: Promise<void>[] = [];
  const settledByIndex: boolean[] = [];
  const expected: string[] = [];
  const received: string[] = [];
  let settled = 0;
  let failure: unknown;

  try {
    router.options.linger = 0;
    dealer.options.linger = 0;
    router.options.immediate = true;
    router.options.mandatory = true;
    router.options.sendHwm = 512n;
    dealer.options.recvHwm = 512n;
    dealer.setRoutingId(peer);
    const address = endpoint();
    router.bind(address);
    dealer.connect(address);

    dealer.send().message('route-ready').submit_sync();
    const ready = new zlink.Received();
    assert.equal(router.recv(ready), true);
    assert.ok(ready.routingId?.equals(peer));
    const target = ready.routingId;
    ready.close();

    for (let index = 0; index < 64; index += 1) {
      const text = `${index.toString().padStart(4, '0')}:${'r'.repeat(59)}`;
      const payload = Buffer.from(text);
      expected.push(text);
      payloads.push(payload);
      settledByIndex.push(false);
      sends.push(router.send(target).message(payload).submit().then(
        () => { settledByIndex[index] = true; settled += 1; },
        (error: unknown) => {
          settledByIndex[index] = true;
          settled += 1;
          failure ??= error;
        }
      ));
    }

    await yieldToEventLoop();
    assert.ok(settled > 0 && settled < sends.length,
      'routed sends must reach HWM after at least one admission');
    const pendingIndex = settledByIndex.findIndex((value) => !value);
    assert.notEqual(pendingIndex, -1);
    payloads[pendingIndex].fill(0x7a);

    for (let turn = 0; turn < 10_000; turn += 1) {
      drainAvailable(dealer, received);
      if (settled === sends.length && received.length === sends.length) break;
      await yieldToEventLoop();
    }
    assert.equal(settled, sends.length);
    assert.equal(received.length, sends.length);
    await Promise.all(sends);
    if (failure !== undefined) throw failure;
    assert.deepEqual(received, expected,
      'managed routed retry must preserve both submit-time payload and target');
  } finally {
    dealer.close();
    router.close();
    context.close();
  }
});

test('CompletionKind exposes the writable wait notification', () => {
  assert.equal(zlink.CompletionKind.Send, 1);
  assert.equal(zlink.CompletionKind.Request, 2);
  assert.equal(zlink.CompletionKind.Writable, 3);
  assert.equal(Object.isFrozen(zlink.CompletionKind), true);
});
