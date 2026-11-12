// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

const zlink = require('@zlink-systems/zlink');
const nativeModule = require(path.resolve(__dirname, '../../dist/zlink/runtime/native/native.js'));
const handles = require(path.resolve(__dirname, '../../dist/zlink/runtime/handles/native_handle.js'));

function settledWithin<T>(promise: Promise<T>, ms: number): Promise<unknown> {
  return new Promise(resolve => {
    const timer = setTimeout(() => resolve('pending'), ms);
    promise.then(
      () => { clearTimeout(timer); resolve('resolved'); },
      (error: unknown) => { clearTimeout(timer); resolve(error); });
  });
}

// Core returns SHUTDOWN when a close of the same handle was already accepted.
// The socket is finished then: pending work ends before the error is reported.
test('socket close reported as SHUTDOWN settles pending requests before throwing', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const received = new zlink.Received();
  const poller = zlink.createPoller();
  router.bind('inproc://close-shutdown');
  dealer.connect('inproc://close-shutdown');
  poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 1);
  try {
    const pending = dealer.request().message('pending').timeout(5000).submit().reply;
    assert.equal(router.recv(received), true);
    received.close();
    poller.remove(dealer);

    nativeModule.requireNative().socketClose(handles.getNativeHandle(dealer));
    assert.throws(() => dealer.close(), (error: any) =>
      error instanceof zlink.CloseError && error.result === zlink.CloseResult.Shutdown);

    const outcome: any = await settledWithin(pending, 1000);
    assert.ok(outcome instanceof zlink.RequestError, String(outcome));
    assert.equal(outcome.result, zlink.RequestResult.Terminated);
    dealer.close();
  } finally {
    poller.close();
    received.close();
    try { dealer.close(); } catch { /* reported by the assertions above */ }
    router.close();
    ctx.close();
  }
});
