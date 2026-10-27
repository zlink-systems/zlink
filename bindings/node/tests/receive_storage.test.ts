// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import { CompletionPollerDriver } from './completion_poller';

const zlink = require('@zlink-systems/zlink');
const payloads = [Buffer.alloc(21, 1), Buffer.alloc(0), Buffer.alloc(1024, 2)];

function send(socket: any): void {
  socket.send().message(payloads[0]).message(payloads[1]).message(payloads[2]).submit_sync();
}

function assertAllocatorStorage(parts: any[], allocated: Buffer[]): void {
  const views = parts.map((part) => part.data() as Buffer);
  assert.deepEqual(views, payloads);
  // Check actual allocator results, rather than platform-specific heap sizes.
  for (const view of views) assert.ok(allocated.includes(view));
  assert.equal(allocated.filter((buffer) => views.some((view) => view === buffer)).length, payloads.length);
}

test('receive adopts standard allocator storage and writes every payload byte', () => {
  const context = zlink.createContext();
  const left = zlink.createPairSocket(context);
  const right = zlink.createPairSocket(context);
  const received = new zlink.Received();
  const original = Buffer.allocUnsafe;
  const allocated: Buffer[] = [];
  try {
    left.bind('inproc://receive-allocator'); right.connect('inproc://receive-allocator');
    Buffer.allocUnsafe = ((size: number) => {
      const buffer = original(size).fill(0xa5);
      allocated.push(buffer);
      return buffer;
    }) as typeof Buffer.allocUnsafe;
    assert.equal(left.recv(received, zlink.RecvFlags.DontWait), false);
    assert.equal(allocated.length, 0, 'NO_DATA must not allocate payload storage');
    send(right);
    assert.equal(left.recv(received), true);
    assert.equal(allocated.length, payloads.length);
    assertAllocatorStorage(received.parts, allocated);
    received.parts[0].close();
    assert.deepEqual(received.parts[2].data(), payloads[2]);
  } finally {
    Buffer.allocUnsafe = original;
    received.close(); right.close(); left.close(); context.close();
  }
});

test('receive backing allocations match the standard allocator baseline', (t) => {
  const records = 64;
  const baseline = new Set<ArrayBufferLike>();
  for (let index = 0; index < records; ++index) {
    for (const payload of payloads) {
      const buffer = Buffer.allocUnsafe(payload.length);
      if (buffer.length > 0) baseline.add(buffer.buffer);
    }
  }
  const context = zlink.createContext();
  const left = zlink.createPairSocket(context);
  const right = zlink.createPairSocket(context);
  const received = new zlink.Received();
  const actual = new Set<ArrayBufferLike>();
  try {
    left.bind('inproc://receive-allocation-count'); right.connect('inproc://receive-allocation-count');
    for (let index = 0; index < records; ++index) {
      send(right);
      assert.equal(left.recv(received), true);
      assert.deepEqual(received.parts.map((part: any) => part.data()), payloads);
      for (const part of received.parts) {
        const buffer = part.data();
        if (buffer.length > 0) actual.add(buffer.buffer);
      }
    }
    t.diagnostic(JSON.stringify({ records, standard: baseline.size, receive: actual.size }));
    // The two runs may begin at different offsets in Node's current slab.
    assert.ok(actual.size <= baseline.size + 1);
  } finally {
    received.close(); right.close(); left.close(); context.close();
  }
});

test('request completions and routed refills adopt standard allocator storage', async () => {
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const driver = new CompletionPollerDriver(context, dealer);
  const received = new zlink.Received();
  const original = Buffer.allocUnsafe;
  const allocated: Buffer[] = [];
  try {
    router.bind('inproc://completion-allocator'); dealer.connect('inproc://completion-allocator');
    Buffer.allocUnsafe = ((size: number) => {
      const buffer = original(size).fill(0xa5);
      allocated.push(buffer);
      return buffer;
    }) as typeof Buffer.allocUnsafe;
    for (let round = 0; round < 2; ++round) {
      const submission = dealer.request().timeout(1000)
        .message(payloads[0]).message(payloads[1]).message(payloads[2]).submit();
      assert.equal(router.recv(received), true);
      assert.deepEqual(received.parts.map((part: any) => part.data()), payloads);
      if (round > 0) assertAllocatorStorage(received.parts, allocated);
      received.reply().message(payloads[0]).message(payloads[1]).message(payloads[2]).submit();
      const parts = await driver.settle<any[]>(submission.reply);
      try { assertAllocatorStorage(parts, allocated); }
      finally { for (const part of parts) part.close(); }
    }
  } finally {
    Buffer.allocUnsafe = original;
    driver.close(); received.close(); dealer.close(); router.close(); context.close();
  }
});

test('allocator reentry preserves both receive records', () => {
  const context = zlink.createContext();
  const left = zlink.createPairSocket(context);
  const right = zlink.createPairSocket(context);
  const otherLeft = zlink.createPairSocket(context);
  const otherRight = zlink.createPairSocket(context);
  const received = new zlink.Received();
  const otherReceived = new zlink.Received();
  const original = Buffer.allocUnsafe;
  let reentered = false;
  try {
    left.bind('inproc://allocator-reentry'); right.connect('inproc://allocator-reentry');
    otherLeft.bind('inproc://allocator-nested'); otherRight.connect('inproc://allocator-nested');
    send(right);
    otherRight.send().message('nested').message('record').submit_sync();
    Buffer.allocUnsafe = ((size: number) => {
      if (!reentered) {
        reentered = true;
        assert.equal(otherLeft.recv(otherReceived), true);
      }
      return original(size);
    }) as typeof Buffer.allocUnsafe;
    assert.equal(left.recv(received), true);
    assert.equal(reentered, true);
    assert.deepEqual(received.parts.map((part: any) => part.data()), payloads);
    assert.deepEqual(otherReceived.parts.map((part: any) => part.getString()), ['nested', 'record']);
  } finally {
    Buffer.allocUnsafe = original;
    otherReceived.close(); received.close(); otherRight.close(); otherLeft.close();
    right.close(); left.close(); context.close();
  }
});

test('an undersized allocator result fails safely and releases the record', () => {
  const context = zlink.createContext();
  const left = zlink.createPairSocket(context);
  const right = zlink.createPairSocket(context);
  const received = new zlink.Received();
  const original = Buffer.allocUnsafe;
  try {
    left.bind('inproc://allocator-failure'); right.connect('inproc://allocator-failure');
    send(right);
    Buffer.allocUnsafe = (() => Buffer.alloc(0)) as typeof Buffer.allocUnsafe;
    assert.throws(() => left.recv(received));
    Buffer.allocUnsafe = original;
    send(right);
    assert.equal(left.recv(received), true);
    assert.deepEqual(received.parts.map((part: any) => part.data()), payloads);
  } finally {
    Buffer.allocUnsafe = original;
    received.close(); right.close(); left.close(); context.close();
  }
});
