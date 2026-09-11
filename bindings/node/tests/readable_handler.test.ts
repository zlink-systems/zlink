// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { constants } from 'node:os';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const zlink = require('@zlink-systems/zlink');
const { completionOwnerOf } = require('../../dist/zlink/runtime/messaging/completion_owner');

test('readable handler replaces its predecessor and drains a queued batch through no data', async () => {
  const ctx = zlink.createContext();
  const sender = zlink.createPairSocket(ctx);
  const receiver = zlink.createPairSocket(ctx);
  const received = new zlink.Received();
  sender.bind('inproc://readable-handler-batch');
  receiver.connect('inproc://readable-handler-batch');
  let replacedCalls = 0;
  let drainedToNoData = false;
  const values: string[] = [];
  try {
    receiver.setReadableHandler(() => { replacedCalls += 1; });
    const ready = new Promise<void>((resolve, reject) => {
      receiver.setReadableHandler(function () {
        try {
          assert.equal(arguments.length, 0);
          while (receiver.recv(received, zlink.RecvFlags.DontWait)) {
            values.push(received.parts[0].getString());
            received.close();
          }
          drainedToNoData = true;
          if (values.length === 4) resolve();
        } catch (error) { reject(error); }
      });
    });
    for (let index = 0; index < 4; index += 1) sender.send().message(String(index)).submit_sync();
    await ready;
    assert.deepEqual(values, ['0', '1', '2', '3']);
    assert.equal(drainedToNoData, true);
    assert.equal(replacedCalls, 0);
  } finally {
    received.close(); receiver.close(); sender.close(); ctx.close();
  }
});

test('readable handler alone keeps the event loop alive and socket close releases it', () => {
  const packagePath = path.resolve(__dirname, '../../dist');
  const child = spawnSync(process.execPath, ['-e', `
    const assert = require('node:assert/strict');
    const z = require(${JSON.stringify(packagePath)});
    const ctx = z.createContext();
    const sender = z.createPairSocket(ctx);
    const receiver = z.createPairSocket(ctx);
    const received = new z.Received();
    sender.bind('inproc://readable-handler-lifetime');
    receiver.connect('inproc://readable-handler-lifetime');
    receiver.setReadableHandler(function () {
      assert.equal(arguments.length, 0);
      let delivered = false;
      while (receiver.recv(received, z.RecvFlags.DontWait)) {
        assert.equal(received.parts[0].getString(), 'wake');
        received.close();
        delivered = true;
      }
      if (delivered) {
        received.close(); receiver.close(); sender.close(); ctx.close();
        process.stdout.write('closed');
      }
    });
    setTimeout(() => sender.send().message('wake').submit_sync(), 25).unref();
  `], { encoding: 'utf8', timeout: 5000 });
  assert.equal(child.error, undefined, child.error?.message);
  assert.equal(child.status, 0, child.stderr);
  assert.equal(child.stdout, 'closed', child.stderr);
});

for (const paused of [false, true]) {
  test(`holding readable DATA for an application permit does not repeat idle notifications (paused=${paused})`, async () => {
    const ctx = zlink.createContext();
    const sender = zlink.createRouterSocket(ctx);
    const receiver = zlink.createDealerSocket(ctx);
    const received = new zlink.Received();
    const rid = zlink.RoutingId.from(Buffer.from('held-receiver'));
    receiver.setRoutingId(rid);
    sender.bind(`inproc://readable-held-${paused}`);
    receiver.connect(`inproc://readable-held-${paused}`);
    let notifications = 0;
    let armed = false;
    let ready: () => void;
    let arm: () => void;
    const initial = new Promise<void>(resolve => { arm = resolve; });
    const readable = new Promise<void>(resolve => { ready = resolve; });
    try {
      receiver.setReadableHandler(() => {
        if (!armed) {
          while (receiver.recv(received, zlink.RecvFlags.DontWait)) received.close();
          armed = true;
          arm();
          return;
        }
        notifications += 1;
        ready();
      });
      await initial;
      sender.send(rid).message('held').submit_sync();
      await readable;
      if (paused) receiver.setReceiveFlowState(zlink.ReceiveFlowState.Paused);
      await delay(10);
      const notificationsAfterProgress = notifications;
      await delay(20);
      assert.equal(notifications, notificationsAfterProgress,
        'without new socket progress, an unread record must not keep waking the event loop');
      assert.equal(receiver.recv(received, zlink.RecvFlags.DontWait), true);
      assert.equal(received.parts[0].getString(), 'held');
      received.close();
      assert.equal(receiver.recv(received, zlink.RecvFlags.DontWait), false);
    } finally { received.close(); receiver.close(); sender.close(); ctx.close(); }
  });
}

test('watch acknowledgement preserves typed request termination during context shutdown', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const request = new zlink.Received();
  const incoming = new zlink.Received();
  const receiveFailures: unknown[] = [];
  let initialized: () => void;
  const initial = new Promise<void>(resolve => { initialized = resolve; });
  router.bind('inproc://readable-context-shutdown');
  dealer.connect('inproc://readable-context-shutdown');
  try {
    dealer.setReadableHandler(() => {
      try {
        while (dealer.recv(incoming, zlink.RecvFlags.DontWait)) incoming.close();
      } catch (error) { receiveFailures.push(error); }
      initialized();
    });
    await initial;
    const pending = dealer.request().message('pending').timeout(1000).submit().reply;
    const rejected = assert.rejects(pending, (error: any) =>
      error instanceof zlink.RequestError && error.result === zlink.RequestResult.Terminated);
    assert.equal(router.recv(request), true);
    request.close();
    ctx.shutdown();
    await rejected;
    assert.ok(receiveFailures.some(error => error instanceof zlink.RecvError));
  } finally {
    request.close(); incoming.close(); dealer.close(); router.close(); ctx.close();
  }
});

test('one readable watch serves replacement handlers and completion ownership transfers', () => {
  const ctx = zlink.createContext();
  const socket = zlink.createPairSocket(ctx);
  const owner = completionOwnerOf(socket) as any;
  let wake: (status: number) => void;
  let starts = 0;
  let stops = 0;
  let calls = 0;
  owner.native = {
    socketReadableWatchStart: (_handle: unknown, callback: (status: number) => void) => {
      starts += 1;
      wake = callback;
      return {};
    },
    socketReadableWatchStop: () => { stops += 1; },
  };
  try {
    socket.setReadableHandler(() => { calls += 100; });
    socket.setReadableHandler(() => { calls += 1; });
    const publicOwner = {};
    owner.transferToPublic(publicOwner);
    wake(0);
    owner.transferToRuntime(publicOwner);
    wake(0);
    assert.equal(starts, 1);
    assert.equal(stops, 0);
    assert.equal(calls, 2);
    assert.throws(() => socket.setReadableHandler(null), (error: any) =>
      error instanceof zlink.HandlerError && error.result === zlink.HandlerResult.InvalidArgument);
    socket.close();
    wake(0);
    assert.equal(calls, 2);
    assert.equal(stops, 1);
    assert.throws(() => socket.setReadableHandler(() => {}), (error: any) =>
      error instanceof zlink.HandlerError && error.result === zlink.HandlerResult.InvalidHandle);
  } finally { socket.close(); ctx.close(); }
});

test('readable notifications coexist with runtime and public request completion drains', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const request = new zlink.Received();
  const data = new zlink.Received();
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  router.bind('inproc://readable-handler-completions');
  dealer.connect('inproc://readable-handler-completions');
  let served: () => void;
  let delivered: () => void;
  let failServing: (error: unknown) => void;
  let failDelivery: (error: unknown) => void;
  const values: string[] = [];
  try {
    router.setReadableHandler(() => {
      try {
        while (router.recv(request, zlink.RecvFlags.DontWait)) {
          const value = request.parts[0].getString();
          const peer = request.routingId;
          request.reply().message(`reply:${value}`).submit();
          request.close();
          router.send(peer).message(`data:${value}`).submit_sync();
          served();
        }
      } catch (error) { failServing(error); }
    });
    dealer.setReadableHandler(() => {
      try {
        while (dealer.recv(data, zlink.RecvFlags.DontWait)) {
          values.push(data.parts[0].getString());
          data.close();
          delivered();
        }
      } catch (error) { failDelivery(error); }
    });
    for (const mode of ['public', 'runtime']) {
      const servedRequest = new Promise<void>((resolve, reject) => { served = resolve; failServing = reject; });
      const receivedData = new Promise<void>((resolve, reject) => { delivered = resolve; failDelivery = reject; });
      const pending = dealer.request().message(mode).timeout(1000).submit().reply;
      if (mode === 'public') poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 7);
      await servedRequest;
      if (mode === 'public') {
        assert.equal(poller.wait(events, 1000), 1);
        assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollCompletion), true);
      }
      const parts = await pending;
      try { assert.equal(parts[0].getString(), `reply:${mode}`); }
      finally { parts.forEach((part: any) => part.close()); }
      await receivedData;
      if (mode === 'public') assert.equal(poller.remove(dealer), true);
    }
    assert.deepEqual(values, ['data:public', 'data:runtime']);
  } finally {
    events.close(); poller.close(); request.close(); data.close();
    dealer.close(); router.close(); ctx.close();
  }
});

test('watch failure rejects all pending operations and reaches the handler only through its next receive', async () => {
  const ctx = zlink.createContext();
  const socket = zlink.createPairSocket(ctx);
  const received = new zlink.Received();
  const owner = completionOwnerOf(socket) as any;
  let wake: (status: number) => void;
  let calls = 0;
  let stops = 0;
  let requestCount = 0;
  owner.native = {
    socketReadableWatchStart: (_handle: unknown, callback: (status: number) => void) => { wake = callback; return {}; },
    socketReadableWatchStop: () => { stops += 1; },
    socketSubmitSend: () => ({ result: zlink.SubmitResult.Backpressured, nativeErrno: constants.errno.EAGAIN, completionId: 11n }),
    socketSubmitRequest: () => ++requestCount === 1
      ? { result: zlink.SubmitResult.Ok, nativeErrno: 0, completionId: 12n }
      : { result: zlink.SubmitResult.Backpressured, nativeErrno: constants.errno.EAGAIN, completionId: 13n },
  };
  try {
    socket.setReadableHandler(function () {
      calls += 1;
      assert.equal(arguments.length, 0);
      assert.throws(() => socket.recv(received, zlink.RecvFlags.DontWait), (error: any) =>
        error instanceof zlink.RecvError && error.result === zlink.RecvResult.InternalError
        && error.nativeErrno === -9 && /readable watch failed/.test(error.message));
      assert.equal(socket.recv(received, zlink.RecvFlags.DontWait), false);
    });
    const send = owner.submitSend(Buffer.from('send'), null);
    const request = owner.submitRequest(Buffer.from('request'), null, 1000);
    const queuedRequest = owner.submitRequest(Buffer.from('queued'), null, 1000);
    const submitFailed = (error: any) => error instanceof zlink.SubmitError
      && error.result === zlink.SubmitResult.InternalError;
    const failed = [
      assert.rejects(send.admitted, submitFailed),
      assert.rejects(request.reply, (error: any) => error instanceof zlink.RequestError
        && error.result === zlink.RequestResult.InternalError),
      assert.rejects(queuedRequest.admitted, submitFailed),
      assert.rejects(queuedRequest.reply, submitFailed),
    ];
    assert.equal(socket.recv(received, zlink.RecvFlags.DontWait), false);
    wake(-9);
    await Promise.all(failed);
    assert.equal(calls, 1);
    assert.equal(stops, 1);
    assert.equal(owner.hasManagedWritableWait(), false);
    socket.close();
    wake(0);
    assert.equal(calls, 1);
  } finally { received.close(); socket.close(); ctx.close(); }
});

for (const [factory, receive, resultType] of [
  ['createPairSocket', 'recv', 'Received'],
  ['createDealerSocket', 'recv', 'Received'],
  ['createRouterSocket', 'recv', 'Received'],
  ['createStreamSocket', 'recv', 'Received'],
  ['createStreamSocket', 'recvPacket', 'StreamPacket'],
  ['createSubSocket', 'subscribe', 'TopicMessage'],
  ['createXSubSocket', 'subscribe', 'TopicMessage'],
  ['createXPubSocket', 'receiveSubscriptionEvent', 'SubscriptionEvent'],
]) {
  test(`${factory}.${receive} consumes its watch failure once without stale errno remapping`, () => {
    const ctx = zlink.createContext();
    const socket = zlink[factory](ctx);
    const result = new zlink[resultType]();
    const owner = completionOwnerOf(socket) as any;
    let wake: (status: number) => void;
    owner.native = {
      socketReadableWatchStart: (_handle: unknown, callback: (status: number) => void) => { wake = callback; return {}; },
      socketReadableWatchStop: () => {},
    };
    try {
      if (factory === 'createStreamSocket') {
        socket.options.recvMode = receive === 'recvPacket'
          ? zlink.StreamRecvMode.Packet : zlink.StreamRecvMode.Raw;
      }
      socket.setReadableHandler(() => {});
      assert.equal(socket[receive](result, zlink.RecvFlags.DontWait), false);
      wake(-9);
      assert.throws(() => socket[receive](result, zlink.RecvFlags.DontWait), (error: any) =>
        error instanceof zlink.RecvError && error.result === zlink.RecvResult.InternalError
        && error.nativeErrno === -9);
      assert.equal(socket[receive](result, zlink.RecvFlags.DontWait), false);
    } finally {
      if (typeof result.close === 'function') result.close();
      socket.close(); ctx.close();
    }
  });
}
