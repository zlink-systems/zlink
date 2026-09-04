// SPDX-License-Identifier: MPL-2.0

'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

const addonPath = path.resolve(__dirname, '../../build/Release/zlink.node');
const native = require(addonPath) as Record<string, (...args: any[]) => any>;
const zlink = require('@zlink-systems/zlink');

let sequence = 0;

function endpoint(label: string): string {
  return `inproc://node-completion-boundary-${label}-${process.pid}-${++sequence}`;
}

test('native completion recv closes an unclaimed late completion exactly once', async () => {
  native.testCompletionCloseCount(true);
  const context = zlink.createContext();
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  router.bind('inproc://late-completion-cleanup');
  dealer.connect('inproc://late-completion-cleanup');
  try {
    const submitted = native.socketSubmitRequest(
      dealer._native,
      null,
      Buffer.from('late'),
      1_000,
      zlink.SendFlags.DontWait,
      999n
    );
    assert.equal(submitted.result, zlink.SubmitResult.Ok);
    assert.notEqual(submitted.completionId, 0n);
    const request = new zlink.Received();
    assert.equal(router.recv(request), true);
    request.reply().message('ignored').submit();

    let completion = null;
    for (let attempt = 0; attempt < 100 && !completion; attempt += 1) {
      completion = native.socketCompletionRecv(dealer._native, zlink.RecvFlags.DontWait);
      if (!completion) await new Promise((resolve) => setImmediate(resolve));
    }
    assert.ok(completion);
    assert.equal(completion.userContext, 999n);
    assert.equal(native.testCompletionCloseCount(false), 1n);
    assert.equal(native.socketCompletionRecv(dealer._native, zlink.RecvFlags.DontWait), null);
    assert.equal(native.testCompletionCloseCount(false), 1n);
    request.close();
  } finally {
    dealer.close(); router.close(); context.close();
  }
});

test('completion boundary exposes pull records without callback functions', () => {
  for (const name of [
    'socketSubmitSend',
    'socketSubmitRequest',
    'socketRequestSync',
    'socketCompletionRecv',
  ]) assert.equal(typeof native[name], 'function', name);
  assert.equal(native.socketSendCompletionHandler, undefined);
  assert.equal(native.socketRequestCompletionHandler, undefined);
});

test('DONTWAIT routed HWM refusal preserves RID through writable-token retry', () => {
  const context = zlink.createContext();
  context.options.autoHwmEnabled = false;
  const router = zlink.createRouterSocket(context);
  const dealer = zlink.createDealerSocket(context);
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  const peer = zlink.RoutingId.from('node-native-writable-peer');
  const peerBytes = peer.toBytes();
  const userContext = 0xB79n;
  let blockedPayload: Buffer | null = null;
  let waitToken = 0n;
  let accepted = 0;

  try {
    router.options.linger = 0;
    dealer.options.linger = 0;
    router.options.immediate = true;
    router.options.mandatory = true;
    router.options.sendHwm = 512n;
    dealer.options.recvHwm = 512n;
    dealer.setRoutingId(peer);
    const address = endpoint('writable');
    router.bind(address);
    dealer.connect(address);

    dealer.send().message('route-ready').submit_sync();
    const routeReady = new zlink.Received();
    assert.equal(router.recv(routeReady), true);
    assert.ok(routeReady.routingId?.equals(peer));
    routeReady.close();

    // POLLOUT is a WRITABLE-token wake, not a connection-ready signal.
    poller.add(router, [zlink.PollEventFlag.PollOut], 79);
    assert.equal(poller.wait(events, 0), 0);

    for (; accepted < 512; accepted += 1) {
      const payload = Buffer.alloc(64, 0x68);
      payload.writeUInt32LE(accepted, 0);
      const submitted = native.socketSubmitSend(
        router._native,
        payload,
        peerBytes,
        zlink.SendFlags.DontWait,
        userContext
      );
      if (submitted.result === zlink.SubmitResult.Backpressured) {
        assert.equal(submitted.nativeErrno, 11);
        assert.notEqual(submitted.completionId, 0n);
        blockedPayload = payload;
        waitToken = submitted.completionId;
        break;
      }
      assert.equal(submitted.result, zlink.SubmitResult.Ok);
      assert.equal(submitted.nativeErrno, 0);
      assert.equal(submitted.completionId, 0n);
    }

    assert.ok(accepted > 0 && accepted < 512,
      'routed DONTWAIT send must reach physical HWM');
    assert.ok(blockedPayload);
    assert.equal(native.socketCompletionRecv(
      router._native, zlink.RecvFlags.DontWait), null);
    assert.equal(poller.wait(events, 0), 0,
      'the HWM-full socket must not remain writable');

    for (let index = 0; index < accepted; index += 1) {
      const received = new zlink.Received();
      assert.equal(dealer.recv(received), true);
      received.close();
    }

    assert.equal(poller.wait(events, 5_000), 1);
    assert.equal(events.slot(0), 79);
    assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollOut), true);
    const writable = native.socketCompletionRecv(
      router._native, zlink.RecvFlags.DontWait);
    assert.ok(writable);
    assert.equal(writable.kind, zlink.CompletionKind.Writable);
    assert.equal(writable.completionId, waitToken);
    assert.equal(writable.userContext, userContext);
    assert.deepEqual(writable.peerRoutingId, peerBytes);
    assert.equal(writable.sendResult, zlink.SubmitResult.Ok);
    assert.equal(writable.terminalErrno, 0);
    assert.equal(native.socketCompletionRecv(
      router._native, zlink.RecvFlags.DontWait), null);

    const retried = native.socketSubmitSend(
      router._native,
      blockedPayload,
      peerBytes,
      zlink.SendFlags.DontWait,
      userContext
    );
    assert.equal(retried.result, zlink.SubmitResult.Ok);
    assert.equal(retried.nativeErrno, 0);
    assert.equal(retried.completionId, 0n);

    const receivedRetry = new zlink.Received();
    assert.equal(dealer.recv(receivedRetry), true);
    assert.deepEqual(receivedRetry.singlePartOrThrow().data(), blockedPayload);
    receivedRetry.close();
    const duplicate = new zlink.Received();
    assert.equal(dealer.recv(duplicate, zlink.RecvFlags.DontWait), false,
      'the rejected attempt must not have retained or delivered the payload');
    duplicate.close();
    assert.equal(native.socketCompletionRecv(
      router._native, zlink.RecvFlags.DontWait), null,
      'ordinary successful SEND must not publish a completion');
  } finally {
    events.close();
    poller.close();
    dealer.close();
    router.close();
    context.close();
  }
});
