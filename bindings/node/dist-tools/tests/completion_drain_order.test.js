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
function writable(completionId, token) {
    return { kind: 3, completionId, userContext: token, peerRoutingId: null,
        sendResult: 0, terminalErrno: 0, requestResult: 0 };
}
function reply(completionId, token, value) {
    return { kind: 2, completionId, userContext: token, peerRoutingId: null,
        sendResult: 0, terminalErrno: 0, requestResult: 0, parts: [Buffer.from(value)] };
}
for (const kind of ['send', 'request']) {
    (0, node_test_1.default)(`WRITABLE ${kind} retries only after NO_DATA and leaves its new completion for the next drain`, async () => {
        const owner = new CompletionOwner(null);
        const publicOwner = {};
        owner.transferToPublic(publicOwner);
        const completions = [];
        const order = [];
        let submissions = 0;
        const submit = (token) => {
            submissions += 1;
            if (submissions === 1) {
                completions.push(writable(101n, token));
                return { result: SubmitResult.Backpressured, nativeErrno: 11, completionId: 101n };
            }
            order.push(`resubmit-${submissions - 1}`);
            if (submissions === 2) {
                completions.push(writable(103n, token));
                return { result: SubmitResult.Backpressured, nativeErrno: 11, completionId: 103n };
            }
            strict_1.default.equal(submissions, 3);
            if (kind === 'request')
                completions.push(reply(104n, token, 'retried'));
            return { result: SubmitResult.Ok, nativeErrno: 0, completionId: kind === 'send' ? 0n : 104n };
        };
        owner.native = {
            socketSubmitSend: (_handle, parts, _target, flags, token) => {
                strict_1.default.equal(flags, 1);
                strict_1.default.equal(parts.toString(), 'retained');
                return submit(token);
            },
            socketSubmitRequest: (_handle, _target, parts, _timeout, flags, token) => {
                strict_1.default.equal(flags, 1);
                if (parts.toString() === 'other') {
                    completions.push(reply(202n, token, 'other reply'));
                    return { result: SubmitResult.Ok, nativeErrno: 0, completionId: 202n };
                }
                strict_1.default.equal(parts.toString(), 'retained');
                return submit(token);
            },
            socketCompletionRecv: (_handle, flags) => {
                strict_1.default.equal(flags, 1);
                const completion = completions.shift() ?? null;
                order.push(completion ? `completion-${completion.completionId}` : 'NO_DATA');
                return completion;
            },
        };
        try {
            const pending = kind === 'send'
                ? owner.submitSend(Buffer.from('retained'), null)
                : owner.submitRequest(Buffer.from('retained'), null, 1000);
            const other = owner.submitRequest(Buffer.from('other'), null, 1000);
            strict_1.default.equal(owner.drain(publicOwner), 2);
            strict_1.default.deepEqual(order, ['completion-101', 'completion-202', 'NO_DATA', 'resubmit-1']);
            const otherParts = await other;
            try {
                strict_1.default.equal(otherParts[0].getString(), 'other reply');
            }
            finally {
                otherParts.forEach((part) => part.close());
            }
            strict_1.default.equal(completions.length, 1, 'the retry WRITABLE belongs to the next drain');
            strict_1.default.equal(owner.hasManagedWritableWait(), true);
            order.length = 0;
            strict_1.default.equal(owner.drain(publicOwner), 1);
            strict_1.default.deepEqual(order, ['completion-103', 'NO_DATA', 'resubmit-2']);
            if (kind === 'request') {
                strict_1.default.equal(completions.length, 1, 'the admitted reply also belongs to the next drain');
                strict_1.default.equal(owner.drain(publicOwner), 1);
            }
            const parts = await pending;
            if (kind === 'request') {
                try {
                    strict_1.default.equal(parts[0].getString(), 'retried');
                }
                finally {
                    parts.forEach((part) => part.close());
                }
            }
            strict_1.default.equal(owner.hasManagedWritableWait(), false);
        }
        finally {
            owner.close();
        }
    });
}
(0, node_test_1.default)('WRITABLE returned by the existing sync native bridge also waits for the owner NO_DATA boundary', async () => {
    const owner = new CompletionOwner(null);
    const completions = [];
    const order = [];
    let sendToken = 0n;
    owner.native = {
        socketReadableWatchStart: () => ({}),
        socketReadableWatchStop: () => { },
        socketSubmitSend: (_handle, _parts, _target, _flags, token) => {
            if (sendToken === 0n) {
                sendToken = token;
                return { result: SubmitResult.Backpressured, nativeErrno: 11, completionId: 301n };
            }
            order.push('resubmit');
            return { result: SubmitResult.Ok, nativeErrno: 0, completionId: 0n };
        },
        socketSubmitRequest: (_handle, _target, _parts, _timeout, _flags, token) => {
            completions.push(reply(302n, token, 'other reply'));
            return { result: SubmitResult.Ok, nativeErrno: 0, completionId: 302n };
        },
        socketRequestSync: () => ({ result: SubmitResult.Ok, nativeErrno: 0, completionId: 303n,
            completions: [writable(301n, sendToken), reply(303n, 0n, 'sync reply')] }),
        socketCompletionRecv: (_handle, flags) => {
            strict_1.default.equal(flags, 1);
            const completion = completions.shift() ?? null;
            order.push(completion ? 'other completion' : 'NO_DATA');
            return completion;
        },
    };
    try {
        const send = owner.submitSend(Buffer.from('retained'), null);
        const other = owner.submitRequest(Buffer.from('other'), null, 1000);
        const syncParts = owner.requestSync(Buffer.from('sync'), null, 1000);
        try {
            strict_1.default.equal(syncParts[0].getString(), 'sync reply');
        }
        finally {
            syncParts.forEach((part) => part.close());
        }
        strict_1.default.deepEqual(order, ['other completion', 'NO_DATA', 'resubmit']);
        await send;
        const otherParts = await other;
        try {
            strict_1.default.equal(otherParts[0].getString(), 'other reply');
        }
        finally {
            otherParts.forEach((part) => part.close());
        }
    }
    finally {
        owner.close();
    }
});
