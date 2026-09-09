// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import vm from 'node:vm';

const nativeErrorsPath = require.resolve('../../dist/zlink/runtime/errors/native_errors');
const nativeErrors = require(nativeErrorsPath);
const { CompletionOwner } = require('../../dist/zlink/runtime/messaging/completion_owner');
const { SubmitResult } = require('../../dist/zlink/contracts/errors/errors');

// Evaluate the real errno boundary with each OS's constants. Do not treat both
// 11 and 35 as EAGAIN: they exchange meanings with EDEADLK on Linux and Darwin.
for (const [platform, again, deadlock] of [['linux', 11, 35], ['darwin', 35, 11]] as const) {
  test(`${platform} EAGAIN preserves send/request wait tokens through repeated backpressure`, async () => {
    const localRequire = createRequire(nativeErrorsPath);
    const exports: any = {};
    vm.runInNewContext(fs.readFileSync(nativeErrorsPath, 'utf8'), {
      exports,
      require: (name: string) => name === 'node:os'
        ? { constants: { errno: { EAGAIN: again, EDEADLK: deadlock } } }
        : localRequire(name),
    }, { filename: nativeErrorsPath });
    assert.equal(exports.isWouldBlock(again), true);
    assert.equal(exports.isWouldBlock(deadlock), false);
    assert.equal(exports.isWouldBlock(0), false);

    const original = nativeErrors.isWouldBlock;
    nativeErrors.isWouldBlock = exports.isWouldBlock;
    try {
      for (const kind of ['send', 'request']) {
        const owner = new CompletionOwner(null) as any;
        const publicOwner = {};
        owner.transferToPublic(publicOwner);
        const queue: any[] = [];
        let calls = 0;
        const submit = (token: bigint) => {
          if (++calls <= 2) {
            const completionId = BigInt(calls);
            queue.push({ kind: 3, completionId, userContext: token,
              sendResult: 0, terminalErrno: 0, requestResult: 0 });
            return { result: SubmitResult.Backpressured, nativeErrno: again, completionId };
          }
          if (kind === 'request') queue.push({ kind: 2, completionId: 3n,
            userContext: token, requestResult: 0, parts: [] });
          return { result: SubmitResult.Ok, nativeErrno: 0,
            completionId: kind === 'send' ? 0n : 3n };
        };
        owner.native = {
          socketSubmitSend: (_h: unknown, _p: unknown, _r: unknown, _f: number, token: bigint) => submit(token),
          socketSubmitRequest: (_h: unknown, _r: unknown, _p: unknown, _t: number, _f: number, token: bigint) => submit(token),
          socketCompletionRecv: () => queue.shift() ?? null,
        };
        try {
          const pending = kind === 'send'
            ? owner.submitSend(Buffer.from('retained'), null)
            : owner.submitRequest(Buffer.from('retained'), null, 1000);
          assert.equal(owner.hasManagedWritableWait(), true);
          owner.drain(publicOwner);
          assert.equal(owner.hasManagedWritableWait(), true, 'second refusal keeps the replacement token');
          owner.drain(publicOwner);
          if (kind === 'request') owner.drain(publicOwner);
          await pending;
          assert.equal(calls, 3);
          assert.equal(owner.hasManagedWritableWait(), false);

          // A real missing token must still fail; platform handling must not
          // weaken the binding's validation of the Core contract.
          owner.native.socketSubmitSend = owner.native.socketSubmitRequest = () => ({
            result: SubmitResult.Backpressured, nativeErrno: again, completionId: 0n,
          });
          await assert.rejects(kind === 'send'
            ? owner.submitSend(Buffer.from('invalid'), null)
            : owner.submitRequest(Buffer.from('invalid'), null, 1000),
          { result: SubmitResult.Backpressured, nativeErrno: again });
        } finally { owner.close(); }
      }
    } finally { nativeErrors.isWouldBlock = original; }
  });
}
