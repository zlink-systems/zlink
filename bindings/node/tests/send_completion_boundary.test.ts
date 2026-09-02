// SPDX-License-Identifier: MPL-2.0

'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

const addonPath = path.resolve(__dirname, '../../build/Release/zlink.node');
const native = require(addonPath) as Record<string, (...args: any[]) => any>;
const zlink = require('@zlink-systems/zlink');

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
      if (!completion) await new Promise((resolve) => setTimeout(resolve, 1));
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
