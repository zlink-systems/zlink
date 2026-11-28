// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import { constants } from 'node:os';

const { CompletionOwner } = require('../../dist/zlink/runtime/messaging/completion_owner');
const { Message, SubmitError, SubmitResult } = require('@zlink-systems/zlink');

function isProtocolFailure(error: unknown): boolean {
  return error instanceof SubmitError
    && (error as { result: number }).result === SubmitResult.InternalError
    && (error as { nativeErrno: number }).nativeErrno === constants.errno.EPROTO;
}

for (const kind of ['send', 'request']) {
  for (const retry of [false, true]) {
    test(`${kind} rejects a non-EAGAIN wait token on ${retry ? 'retry' : 'first submit'}`, async () => {
      const owner = new CompletionOwner(null) as any;
      const publicOwner = {};
      owner.transferToPublic(publicOwner);
      const message = Message.from('caller payload');
      let writable: object | null = null;
      let attempts = 0;
      function submit(token: bigint) {
        attempts += 1;
        if (retry && attempts === 1) {
          writable = {
            kind: 3, completionId: 71n, userContext: token,
            peerRoutingId: null, sendResult: 0, terminalErrno: 0, requestResult: 0,
          };
          return { result: SubmitResult.Backpressured, nativeErrno: constants.errno.EAGAIN, completionId: 71n };
        }
        return { result: SubmitResult.Backpressured, nativeErrno: constants.errno.EINVAL, completionId: 72n };
      }
      owner.native = {
        socketSubmitSend: (_handle: unknown, _parts: unknown, _rid: unknown, _flags: number, token: bigint) => submit(token),
        socketSubmitRequest: (_handle: unknown, _rid: unknown, _parts: unknown, _timeout: number, _flags: number, token: bigint) => submit(token),
        socketCompletionRecv: () => {
          const completion = writable;
          writable = null;
          return completion;
        },
      };
      const submitOperation = () => kind === 'send'
        ? owner.submitSend(message, null)
        : owner.submitRequest(message, null, 1000);

      try {
        if (retry) {
          const submission = submitOperation();
          assert.equal(submission.result, SubmitResult.Backpressured);
          assert.equal(message.getString(), '');
          assert.equal(owner.retries.size, 1);
          owner.drain(publicOwner);
          const stages = submission.reply ? [submission.admitted, submission.reply] : [submission.admitted];
          await Promise.all(stages.map((stage: Promise<unknown>) => assert.rejects(stage, isProtocolFailure)));
          assert.equal(message.getString(), '');
        } else {
          assert.throws(submitOperation, isProtocolFailure);
          assert.equal(message.getString(), 'caller payload');
        }
        assert.equal(attempts, retry ? 2 : 1);
        assert.equal(owner.byToken.size, 0);
        assert.equal(owner.byId.size, 0);
        assert.equal(owner.retries.size, 0);
        assert.equal(owner.hasManagedWritableWait(), false);
      } finally {
        owner.close();
        message.close();
      }
    });
  }
}
