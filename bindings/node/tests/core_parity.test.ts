// SPDX-License-Identifier: MPL-2.0

'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { CompletionPollerDriver } from './completion_poller';

const zlink = require('@zlink-systems/zlink');
const nativeModule = require(path.resolve(__dirname, '../../dist/zlink/runtime/native/native.js'));

let sequence = 0;
function endpoint(label: string): string {
  return `inproc://node-core-parity-${label}-${process.pid}-${++sequence}`;
}

function intercept(method: string, replacement: (...args: unknown[]) => unknown): () => void {
  const original = nativeModule.requireNative;
  const native = original();
  nativeModule.requireNative = () => new Proxy({}, {
    get(_target, property) {
      return property === method ? replacement : native[property];
    }
  });
  return () => { nativeModule.requireNative = original; };
}

test('socket close reports Busy at once and leaves the socket open', async () => {
  const context = zlink.createContext();
  const dealer = zlink.createDealerSocket(context);
  const router = zlink.createRouterSocket(context);
  const address = endpoint('close-busy');
  router.bind(address);
  dealer.connect(address);
  const original = nativeModule.requireNative();
  let calls = 0;
  const restore = intercept('socketClose', (...args: unknown[]) => {
    calls += 1;
    if (calls === 1) {
      throw Object.assign(new Error('socket busy'), {
        nativeErrno: 16,
        nativeResult: zlink.CloseResult.Busy,
      });
    }
    return original.socketClose(...args);
  });
  const driver = new CompletionPollerDriver(context, dealer);
  try {
    const pending = dealer.request().message('ping').timeout(5_000).submit();
    assert.throws(() => dealer.close(), (error: any) =>
      error instanceof zlink.CloseError && error.result === zlink.CloseResult.Busy);
    assert.equal(calls, 1, 'close must not retry or wait after Busy');

    // The refused close leaves the request in flight and the socket usable.
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    request.reply().message('pong').submit();
    request.close();
    const reply = await driver.settle(pending.reply);
    assert.equal(reply[0].getString(), 'pong');
    dealer.close();
    assert.equal(calls, 2);
  } finally {
    restore();
    driver.close();
    try { dealer.close(); } catch { /* preserve the assertion */ }
    router.close();
    context.close();
  }
});

for (const kind of ['dealer', 'router'] as const) {
  test(`${kind} request without a timeout passes 0 so Core applies the socket option`, async () => {
    const original = nativeModule.requireNative();
    const timeouts: number[] = [];
    // A socket captures the native module when it is created.
    const restore = intercept('socketSubmitRequest', (...args: unknown[]) => {
      timeouts.push(args[3] as number);
      return original.socketSubmitRequest(...args);
    });
    const context = zlink.createContext();
    const requester = kind === 'dealer'
      ? zlink.createDealerSocket(context)
      : zlink.createRouterSocket(context);
    const peer = zlink.createRouterSocket(context);
    const driver = new CompletionPollerDriver(context, requester);
    try {
      const address = endpoint(`timeout-${kind}`);
      requester.options.requestTimeout = 300;
      if (kind === 'router') {
        requester.setRoutingId(zlink.RoutingId.from('requester'));
        peer.setRoutingId(zlink.RoutingId.from('peer'));
      }
      peer.bind(address);
      requester.connect(address);

      // The peer never replies, so the socket option decides the deadline.
      const started = Date.now();
      const submission = kind === 'dealer'
        ? requester.request().message('q').submit()
        : requester.request(zlink.RoutingId.from('peer')).message('q').submit();
      await assert.rejects(driver.settle(submission.reply, 4_000), (error: any) =>
        error instanceof zlink.RequestError && error.result === zlink.RequestResult.TimedOut);
      assert.ok(Date.now() - started < 3_000, 'the socket option, not a binding default, ends the request');
      assert.deepEqual(timeouts, [0]);
    } finally {
      restore();
      driver.close();
      requester.close();
      peer.close();
      context.close();
    }
  });
}
