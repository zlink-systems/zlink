// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import { constants } from 'node:os';

const { CompletionOwner } = require('../../dist/zlink/runtime/messaging/completion_owner');
const { Message, SubmitResult } = require('@zlink-systems/zlink');

const REQUEST = 2;
const WRITABLE = 3;
const waitToken = 71n;

function completion(kind: number, userContext: bigint) {
  return {
    kind, completionId: waitToken, userContext, peerRoutingId: null,
    sendResult: 0, terminalErrno: 0, requestResult: 0,
    parts: kind === REQUEST ? [Buffer.from('late reply')] : undefined,
  };
}

for (const operation of ['SEND', 'REQUEST']) {
  for (const [recordName, recordKind] of [['WRITABLE', WRITABLE], ['REQUEST completion', REQUEST]] as const) {
    test(`discards late ${recordName} after ${operation} fails with EPROTO`, { timeout: 2000 }, async () => {
      const owner = new CompletionOwner(null) as any;
      const publicOwner = {};
      owner.transferToPublic(publicOwner);
      const rejected = Message.from('rejected payload');
      const next = Buffer.from('next payload');
      let rejectedContext = 0n;
      let nextContext = 0n;
      let nextAttempts = 0;
      let record: ReturnType<typeof completion> | null = null;

      function reject(context: bigint) {
        rejectedContext = context;
        return { result: SubmitResult.Backpressured, nativeErrno: constants.errno.EINVAL, completionId: waitToken };
      }

      owner.native = {
        socketSubmitSend: (_handle: unknown, parts: unknown, _target: unknown, _flags: number, context: bigint) => {
          if (rejectedContext === 0n) return reject(context);

          assert.ok(Buffer.isBuffer(parts));
          assert.equal(parts.toString(), 'next payload');
          nextAttempts += 1;
          if (nextAttempts === 1) {
            nextContext = context;
            assert.notEqual(nextContext, rejectedContext);
            record = completion(recordKind, rejectedContext);
            assert.equal(owner.drain(publicOwner), 1);
            assert.equal(owner.byToken.has(rejectedContext), false);
            assert.equal(owner.byId.has(waitToken), false, 'the late record must be discarded before SEND returns');
            assert.equal(owner.writableRetries.length, 0);
            return { result: SubmitResult.Backpressured, nativeErrno: constants.errno.EAGAIN, completionId: waitToken };
          }
          assert.equal(context, nextContext);
          return { result: SubmitResult.Ok, nativeErrno: 0, completionId: 0n };
        },
        socketSubmitRequest: (_handle: unknown, _target: unknown, _parts: unknown, _timeout: number, _flags: number, context: bigint) => reject(context),
        socketCompletionRecv: () => {
          const queued = record;
          record = null;
          return queued;
        },
      };

      try {
        const submitRejected = operation === 'SEND'
          ? () => owner.submitSend(rejected, null)
          : () => owner.submitRequest(rejected, null, 1000);
        assert.throws(submitRejected, {
          name: 'SubmitError', result: SubmitResult.InternalError, nativeErrno: constants.errno.EPROTO,
        });
        assert.equal(rejected.getString(), 'rejected payload');
        assert.notEqual(rejectedContext, 0n);

        const submission = owner.submitSend(next, null);
        assert.equal(submission.result, SubmitResult.Backpressured);
        assert.equal(nextAttempts, 1);
        assert.equal(owner.hasManagedWritableWait(), true);
        assert.equal(await Promise.race([submission.admitted, Promise.resolve('pending')]), 'pending');

        record = completion(WRITABLE, nextContext);
        assert.equal(owner.drain(publicOwner), 1);
        await submission.admitted;
        assert.equal(nextAttempts, 2);
        assert.equal(owner.byToken.size, 0);
        assert.equal(owner.byId.size, 0);
        assert.equal(owner.hasManagedWritableWait(), false);
      } finally {
        owner.close();
        rejected.close();
      }
    });
  }
}
