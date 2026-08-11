'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

function recvMaybe(socket) {
  const received = new zlink.Received();
  try {
    return socket.recv(received, zlink.RecvFlags.DontWait) ? received : null;
  } catch (error) {
    if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) {
      return null;
    }
    throw error;
  }
}

function recvText(socket) {
  const received = new zlink.Received();
  socket.recv(received);
  return received.parts[0].data().toString();
}

test('message wrapper pool reuses only explicitly returned facades', () => {
  const held = zlink.Message.from('held');
  const returned = zlink.Message.from('returned');

  returned.close();
  const reused = zlink.Message.from('reused');

  assert.strictEqual(reused, returned);
  assert.notStrictEqual(reused, held);
  assert.equal(reused.data().toString(), 'reused');
  assert.equal(held.data().toString(), 'held');

  reused.close();
  held.close();
});

test('consumed message waits for deterministic cleanup before pool reuse', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);
  sender.bind('inproc://message-wrapper-pool-consumed');
  receiver.connect('inproc://message-wrapper-pool-consumed');

  const consumed = zlink.Message.from('consumed');
  sender.send().message(consumed).submit();
  const beforeCleanup = zlink.Message.from('before-cleanup');
  assert.notStrictEqual(beforeCleanup, consumed);

  consumed.close();
  const afterCleanup = zlink.Message.from('after-cleanup');
  assert.strictEqual(afterCleanup, consumed);

  const received = new zlink.Received();
  receiver.recv(received);
  received.close();
  afterCleanup.close();
  beforeCleanup.close();
  receiver.close();
  sender.close();
  ctx.close();
});

test('pair messaging uses Message and Received by default', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);

  sender.bind('inproc://pair-contract');
  receiver.connect('inproc://pair-contract');
  sender.send().message('ping').submit();

  const received = new zlink.Received();
  receiver.recv(received);
  assert.equal(received.parts.length, 1);
  assert.ok(Object.isFrozen(received.parts));
  assert.ok(received.parts[0] instanceof zlink.Message);
  assert.equal(received.parts[0].data().toString(), 'ping');
  assert.equal(received.routingId, null);

  receiver.close();
  sender.close();
  ctx.close();
});

test('poller writes reusable event buffer and dispatches by slot', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(2);

  sender.bind('inproc://node-poller-reusable');
  receiver.connect('inproc://node-poller-reusable');
  poller.add(receiver, [zlink.PollEventFlag.PollIn], 7);
  sender.send().message('ready').submit();

  const count = poller.wait(events, 2000);
  assert.equal(count, 1);
  assert.equal(events.readyCount, 1);
  assert.equal(events.sourceKind(0), 1);
  assert.equal(events.slot(0), 7);
  assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollIn), true);
  assert.throws(() => events.slot(1), RangeError);
  assert.equal(recvText(receiver), 'ready');

  events.close();
  poller.close();
  receiver.close();
  sender.close();
  ctx.close();
});

test('poller capacity limits written events without losing remaining readiness', () => {
  const ctx = zlink.createContext();
  const sender1 = zlink.createPairSocket(ctx);
  const receiver1 = zlink.createPairSocket(ctx);
  const sender2 = zlink.createPairSocket(ctx);
  const receiver2 = zlink.createPairSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);

  sender1.bind('inproc://node-poller-capacity-a');
  receiver1.connect('inproc://node-poller-capacity-a');
  sender2.bind('inproc://node-poller-capacity-b');
  receiver2.connect('inproc://node-poller-capacity-b');
  poller.add(receiver1, [zlink.PollEventFlag.PollIn], 101);
  poller.add(receiver2, [zlink.PollEventFlag.PollIn], 102);
  sender1.send().message('a').submit();
  sender2.send().message('b').submit();

  let count = poller.wait(events, 2000);
  assert.equal(count, 1);
  const firstSlot = events.slot(0);
  assert.ok(firstSlot === 101 || firstSlot === 102);
  if (firstSlot === 101) {
    assert.equal(recvText(receiver1), 'a');
  } else {
    assert.equal(recvText(receiver2), 'b');
  }

  count = poller.wait(events, 2000);
  assert.equal(count, 1);
  assert.notEqual(events.slot(0), firstSlot);
  if (events.slot(0) === 101) {
    assert.equal(recvText(receiver1), 'a');
  } else {
    assert.equal(recvText(receiver2), 'b');
  }

  events.close();
  poller.close();
  receiver2.close();
  sender2.close();
  receiver1.close();
  sender1.close();
  ctx.close();
});

test('poller modify remove and timeout follow core semantics', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);

  sender.bind('inproc://node-poller-modify-remove');
  receiver.connect('inproc://node-poller-modify-remove');
  poller.add(receiver, [zlink.PollEventFlag.PollIn], 31);
  poller.modify(receiver, []);
  sender.send().message('hidden').submit();
  assert.equal(poller.wait(events, 20), 0);

  poller.modify(receiver, [zlink.PollEventFlag.PollIn]);
  assert.equal(poller.wait(events, 2000), 1);
  assert.equal(events.slot(0), 31);
  assert.equal(recvText(receiver), 'hidden');

  assert.equal(poller.remove(receiver), true);
  sender.send().message('removed').submit();
  assert.equal(poller.wait(events, 0), 0);

  events.close();
  poller.close();
  receiver.close();
  sender.close();
  ctx.close();
});

test('poller distinguishes socket and timer events in one buffer', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);
  const timer = zlink.createTimer();
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(2);

  sender.bind('inproc://node-poller-timer-socket');
  receiver.connect('inproc://node-poller-timer-socket');
  poller.add(receiver, [zlink.PollEventFlag.PollIn], 41);
  poller.add(timer, 42);
  sender.send().message('socket').submit();
  timer.start(5_000_000n, 1n);

  let sawSocket = false;
  let sawTimer = false;
  const deadline = Date.now() + 2000;
  while ((!sawSocket || !sawTimer) && Date.now() < deadline) {
    const count = poller.wait(events, 200);
    for (let i = 0; i < count; i += 1) {
      if (events.sourceKind(i) === 1) {
        assert.equal(events.slot(i), 41);
        assert.equal(recvText(receiver), 'socket');
        sawSocket = true;
      } else if (events.sourceKind(i) === 3) {
        assert.equal(events.slot(i), 42);
        assert.equal(timer.recv(), 1n);
        sawTimer = true;
      }
    }
  }

  assert.equal(sawSocket, true);
  assert.equal(sawTimer, true);

  events.close();
  poller.close();
  timer.close();
  receiver.close();
  sender.close();
  ctx.close();
});

test('poller runtime surface excludes allocation wait and wrapper lookups', () => {
  const events = zlink.createPollEvents(1);
  const poller = zlink.createPoller();

  assert.equal(poller.waitMany, undefined);
  assert.equal(events.socket, undefined);
  assert.equal(events.timer, undefined);
  assert.throws(() => zlink.createPollEvents(0), RangeError);
  assert.throws(() => poller.addFd(0, [zlink.PollEventFlag.PollIn], -1), RangeError);

  poller.close();
  events.close();
});

test('recv returns null when no message is available with DontWait', () => {
  const ctx = zlink.createContext();
  const pair = zlink.createPairSocket(ctx);

  assert.equal(recvMaybe(pair), null);

  pair.close();
  ctx.close();
});

test('recvHandler delivers multipart Message instances', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);

  sender.bind('inproc://pair-handler-contract');
  receiver.connect('inproc://pair-handler-contract');

  sender.send().message('left').message('right').submit();
  const received = new zlink.Received();
  receiver.recv(received);

  assert.equal(received.routingId, null);
  assert.deepEqual(
    received.parts.map((part) => part.data().toString()),
    ['left', 'right']
  );

  receiver.close();
  sender.close();
  ctx.close();
});

test('pair supports canonical builder send and caller-provided recv storage', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);

  sender.bind('inproc://pair-buffer-fast-path');
  receiver.connect('inproc://pair-buffer-fast-path');

  sender.send().message(Buffer.from('buffer-ping')).submit();
  const first = new zlink.Received();
  assert.equal(receiver.recv(first), true);
  assert.equal(first.singlePartOrThrow().data().toString(), 'buffer-ping');

  sender.send().message(Buffer.from('buffer-pong')).submit();
  const second = new zlink.Received();
  assert.equal(receiver.recv(second), true);
  assert.equal(second.singlePartOrThrow().data().toString(), 'buffer-pong');

  receiver.close();
  sender.close();
  ctx.close();
});

test('pair single-part recv preserves payload semantics while reusing storage', () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);

  sender.bind('inproc://pair-single-part-reuse');
  receiver.connect('inproc://pair-single-part-reuse');

  const received = new zlink.Received();
  sender.send().message(Buffer.alloc(0)).submit();
  assert.equal(receiver.recv(received), true);
  const previous = received.singlePartOrThrow();
  assert.equal(previous.size(), 0);
  assert.equal(previous.refCount(), 1);
  assert.equal(previous.getProperty('Identity'), null);

  sender.send().message('replacement').submit();
  assert.equal(receiver.recv(received), true);
  assert.equal(received.singlePartOrThrow().data().toString(), 'replacement');

  receiver.close();
  sender.close();
  ctx.close();
});

test('pair surface stays recv-only on the canonical api', () => {
  const ctx = zlink.createContext();
  const receiver = zlink.createPairSocket(ctx);

  assert.equal(receiver.onReceive, undefined);
  assert.equal(typeof receiver.recv, 'function');

  receiver.close();
  ctx.close();
});
