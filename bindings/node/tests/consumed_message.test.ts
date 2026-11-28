// SPDX-License-Identifier: MPL-2.0

import assert from 'node:assert/strict';
import test from 'node:test';
import { CompletionPollerDriver } from './completion_poller';

const zlink = require('@zlink-systems/zlink') as typeof import('../dist/index');
const { Message, Received, RecvFlags, SubmitError, SubmitResult } = zlink;

function isInvalidHandle(error: unknown): boolean {
  return error instanceof SubmitError && error.result === SubmitResult.InvalidHandle;
}

for (const terminal of ['submit', 'submit_sync'] as const) {
  test(`send ${terminal} rejects a consumed part before sending any frames`, () => {
    const context = zlink.createContext();
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    const received = new Received();
    const consumed = Message.from('first');
    const live = Message.from('second');
    const empty = Message.allocate(0);
    try {
      receiver.bind('inproc://consumed-message-send');
      sender.connect('inproc://consumed-message-send');
      sender.send().message(consumed).submit_sync();
      assert.equal(receiver.recv(received, RecvFlags.DontWait), true);
      assert.equal(received.singlePartOrThrow().getString(), 'first');
      assert.deepEqual(consumed.data(), Buffer.alloc(0));
      assert.equal(consumed.size(), 0);

      let error: unknown;
      try {
        sender.send().message(live).message(consumed)[terminal]();
      } catch (failure) {
        error = failure;
      }
      const frames = receiver.recv(received, RecvFlags.DontWait)
        ? received.parts.map(part => part.getString()) : [];
      assert.deepEqual(frames, [], 'rejected send must leave the peer queue empty');
      assert.ok(isInvalidHandle(error), `expected SubmitError.InvalidHandle, got ${error}`);
      assert.equal(live.getString(), 'second');

      sender.send().message(live).message(empty).submit_sync();
      assert.equal(receiver.recv(received, RecvFlags.DontWait), true);
      assert.deepEqual(received.parts.map(part => part.getString()), ['second', '']);
    } finally {
      empty.close(); live.close(); consumed.close(); received.close();
      receiver.close(); sender.close(); context.close();
    }
  });
}

test('send rejects a Message closed after it was added to the builder', () => {
  const context = zlink.createContext();
  const sender = zlink.createPairSocket(context);
  const receiver = zlink.createPairSocket(context);
  const received = new Received();
  const closed = Message.from('closed');
  try {
    receiver.bind('inproc://closed-message-send');
    sender.connect('inproc://closed-message-send');
    const send = sender.send().message(closed);
    closed.close();
    assert.throws(() => send.submit_sync(), isInvalidHandle);
    assert.deepEqual(closed.data(), Buffer.alloc(0));
    assert.equal(closed.size(), 0);
    assert.equal(receiver.recv(received, RecvFlags.DontWait), false);
  } finally {
    closed.close(); received.close();
    receiver.close(); sender.close(); context.close();
  }
});

test('request submit and submit_sync reject a closed part without reaching the router', () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const completions = new CompletionPollerDriver(context, dealer);
  const received = new Received();
  const closed = Message.from('closed');
  closed.close();
  try {
    router.bind('inproc://consumed-message-request');
    dealer.connect('inproc://consumed-message-request');
    assert.throws(() => {
      const pending = dealer.request().message('valid').message(closed).timeout(100).submit();
      void pending.admitted.catch(() => {});
      void pending.reply.catch(() => {});
    }, isInvalidHandle);
    assert.throws(() => dealer.request().message(closed).timeout(100).submit_sync(), isInvalidHandle);
    assert.equal(router.recv(received, RecvFlags.DontWait), false);
  } finally {
    received.close(); completions.close(); dealer.close(); router.close(); context.close();
  }
});

test('router and Received replies reject a closed part and preserve the reply token', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const completions = new CompletionPollerDriver(context, dealer);
  const received = new Received();
  const closed = Message.from('closed');
  const live = Message.from('reply');
  try {
    router.bind('inproc://consumed-message-reply');
    dealer.connect('inproc://consumed-message-reply');
    const pending = dealer.request().message('request').timeout(1_000).submit();
    void pending.reply.catch(() => {});
    assert.equal(router.recv(received, RecvFlags.DontWait), true);
    const { routingId, replyToken } = received;
    assert.ok(routingId && replyToken);
    closed.close();
    assert.throws(() => router.reply(routingId, replyToken).message(closed).submit(), isInvalidHandle);
    assert.throws(() => received.reply().message(live).message(closed).submit(), isInvalidHandle);
    received.reply().message(live).submit();
    const reply = await completions.settle<InstanceType<typeof Message>[]>(pending.reply);
    try {
      assert.deepEqual(reply.map(part => part.getString()), ['reply']);
    } finally {
      for (const part of reply) part.close();
    }
  } finally {
    live.close(); closed.close(); received.close();
    completions.close(); dealer.close(); router.close(); context.close();
  }
});

test('publish rejects a closed part without delivering any frames', () => {
  const context = zlink.createContext();
  const publisher = zlink.createXPubSocket(context);
  const subscriber = zlink.createSubSocket(context);
  const subscription = new zlink.SubscriptionEvent();
  const received = new zlink.TopicMessage();
  const closed = Message.from('closed');
  closed.close();
  try {
    publisher.bind('inproc://consumed-message-publish');
    subscriber.connect('inproc://consumed-message-publish');
    subscriber.setSubscription('topic');
    assert.equal(publisher.receiveSubscriptionEvent(subscription), true);
    assert.throws(() => publisher.publish('topic').message('valid').message(closed).submit(), isInvalidHandle);
    assert.equal(subscriber.subscribe(received, RecvFlags.DontWait), false);
    publisher.publish('topic').message('valid').submit();
    assert.equal(subscriber.subscribe(received, RecvFlags.DontWait), true);
    assert.equal(received.singlePartOrThrow().getString(), 'valid');
  } finally {
    received.close(); subscriber.close(); publisher.close(); context.close();
  }
});

test('reusing a TopicMessage does not revive a part consumed by publish', () => {
  const context = zlink.createContext();
  const publisher = zlink.createXPubSocket(context);
  const subscriber = zlink.createSubSocket(context);
  const subscription = new zlink.SubscriptionEvent();
  const received = new zlink.TopicMessage();
  try {
    publisher.bind('inproc://consumed-topic-part');
    subscriber.connect('inproc://consumed-topic-part');
    subscriber.setSubscription('topic');
    assert.equal(publisher.receiveSubscriptionEvent(subscription), true);
    publisher.publish('topic').message('first').submit();
    assert.equal(subscriber.subscribe(received, RecvFlags.DontWait), true);
    const consumed = received.singlePartOrThrow();
    try {
      publisher.publish('topic').message(consumed).submit();
      assert.equal(subscriber.subscribe(received, RecvFlags.DontWait), true);
      publisher.publish('topic').message('next').submit();
      assert.equal(subscriber.subscribe(received, RecvFlags.DontWait), true);
      assert.equal(received.singlePartOrThrow().getString(), 'next');
      assert.equal(consumed.size(), 0);
      assert.throws(() => publisher.publish('topic').message(consumed).submit(), isInvalidHandle);
    } finally {
      consumed.close();
    }
  } finally {
    received.close(); subscriber.close(); publisher.close(); context.close();
  }
});
