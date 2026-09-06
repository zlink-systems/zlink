// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';

const { CompletionOwner } = require('../../dist/zlink/runtime/messaging/completion_owner');
const { SubmitResult } = require('../../dist/zlink/contracts/errors/errors');

for (const kind of ['send', 'request']) {
  for (const target of [null, Buffer.from('submitted-peer')]) {
    test(`WRITABLE delivers ${kind} by token without reading the ${target ? 'routed' : 'unrouted'} peer echo`, async () => {
      const owner = new CompletionOwner(null) as any;
      const publicOwner = {};
      owner.transferToPublic(publicOwner);
      const completions: any[] = [];
      let submissions = 0;
      const submit = (routingId: Buffer | null, token: bigint) => {
        assert.deepEqual(routingId, target);
        submissions += 1;
        if (submissions === 1) {
          completions.push({
            kind: 3, completionId: 71n, userContext: token,
            get peerRoutingId(): never {
              throw new Error('the binding must not re-judge the Core peer RID echo');
            },
            sendResult: 0, terminalErrno: 0, requestResult: 0,
          });
          return { result: SubmitResult.Backpressured, nativeErrno: 11, completionId: 71n };
        }
        assert.equal(submissions, 2, 'one WRITABLE authorizes one resubmission');
        if (kind === 'request') {
          completions.push({
            kind: 2, completionId: 72n, userContext: token,
            peerRoutingId: target, sendResult: 0, terminalErrno: 0,
            requestResult: 0, parts: [Buffer.from('reply')],
          });
        }
        return { result: SubmitResult.Ok, nativeErrno: 0, completionId: kind === 'send' ? 0n : 72n };
      };
      owner.native = {
        socketSubmitSend: (_handle: unknown, _parts: unknown, rid: Buffer | null, flags: number, token: bigint) => {
          assert.equal(flags, 1);
          return submit(rid, token);
        },
        socketSubmitRequest: (_handle: unknown, rid: Buffer | null, _parts: unknown, _timeout: number, flags: number, token: bigint) => {
          assert.equal(flags, 1);
          return submit(rid, token);
        },
        socketCompletionRecv: (_handle: unknown, flags: number) => {
          assert.equal(flags, 1);
          return completions.shift() ?? null;
        },
      };
      try {
        const pending = kind === 'send'
          ? owner.submitSend(Buffer.from('payload'), target)
          : owner.submitRequest(Buffer.from('payload'), target, 1000);
        owner.drain(publicOwner);
        owner.drain(publicOwner);
        const reply = await pending;
        assert.equal(submissions, 2);
        assert.equal(owner.hasManagedWritableWait(), false);
        if (kind === 'request') {
          try { assert.equal(reply[0].getString(), 'reply'); }
          finally { reply.forEach((part: any) => part.close()); }
        }
      } finally { owner.close(); }
    });
  }
}
