"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const { CompletionOwner } = require('../../dist/zlink/runtime/messaging/completion_owner');
const { SubmitResult } = require('../../dist/zlink/contracts/errors/errors');
for (const kind of ['send', 'request']) {
    for (const target of [null, Buffer.from('submitted-peer')]) {
        (0, node_test_1.default)(`WRITABLE delivers ${kind} by token without reading the ${target ? 'routed' : 'unrouted'} peer echo`, async () => {
            const owner = new CompletionOwner(null);
            const publicOwner = {};
            owner.transferToPublic(publicOwner);
            const completions = [];
            let submissions = 0;
            const submit = (routingId, token) => {
                strict_1.default.deepEqual(routingId, target);
                submissions += 1;
                if (submissions === 1) {
                    completions.push({
                        kind: 3, completionId: 71n, userContext: token,
                        get peerRoutingId() {
                            throw new Error('the binding must not re-judge the Core peer RID echo');
                        },
                        sendResult: 0, terminalErrno: 0, requestResult: 0,
                    });
                    return { result: SubmitResult.Backpressured, nativeErrno: 11, completionId: 71n };
                }
                strict_1.default.equal(submissions, 2, 'one WRITABLE authorizes one resubmission');
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
                socketSubmitSend: (_handle, _parts, rid, flags, token) => {
                    strict_1.default.equal(flags, 1);
                    return submit(rid, token);
                },
                socketSubmitRequest: (_handle, rid, _parts, _timeout, flags, token) => {
                    strict_1.default.equal(flags, 1);
                    return submit(rid, token);
                },
                socketCompletionRecv: (_handle, flags) => {
                    strict_1.default.equal(flags, 1);
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
                strict_1.default.equal(submissions, 2);
                strict_1.default.equal(owner.hasManagedWritableWait(), false);
                if (kind === 'request') {
                    try {
                        strict_1.default.equal(reply[0].getString(), 'reply');
                    }
                    finally {
                        reply.forEach((part) => part.close());
                    }
                }
            }
            finally {
                owner.close();
            }
        });
    }
}
