// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const { CompletionEntry, CompletionOwner, } = require('../../dist/zlink/runtime/messaging/completion_owner');
const { RequestError, RequestResult, SubmitResult, } = require('../../dist/zlink/contracts/errors/errors');
const { mapNativeErrno, } = require('../../dist/zlink/runtime/errors/error_mapping');
function requestCompletion(completionId, userContext) {
    return {
        kind: 2,
        completionId,
        userContext,
        peerRoutingId: null,
        sendResult: 0,
        terminalErrno: 0,
        requestResult: 0,
        parts: [],
    };
}
test('successful send settles without publishing a SEND completion id', async () => {
    const entry = new CompletionEntry(41n, 'send');
    entry.succeed(undefined);
    await entry.promise;
    assert.equal(entry.completionId, 0n);
    assert.equal(entry.published, true);
    assert.equal(entry.captured, true);
    assert.equal(entry.settled, true);
});
test('native context termination maps to a terminated send result', () => {
    assert.equal(mapNativeErrno('submit', 156384765), SubmitResult.Terminated);
});
test('native socket shutdown maps WRITABLE terminal to a terminated send result', () => {
    assert.equal(mapNativeErrno('submit', 108), SubmitResult.Terminated);
});
test('missing routed target maps WRITABLE terminal to a not-found send result', () => {
    assert.equal(mapNativeErrno('submit', 2), SubmitResult.NotFound);
});
test('native context termination preserves REQUEST terminated semantics', () => {
    assert.equal(mapNativeErrno('request', 156384765), RequestResult.Terminated);
});
test('unknown nonzero context never falls back to a different live completion id', async () => {
    const owner = new CompletionOwner(null);
    const entry = owner.register('request', false);
    const ignoredRejection = entry.promise.catch(() => undefined);
    owner.publish(entry, 501n);
    owner.capture(requestCompletion(501n, 999n));
    assert.equal(entry.captured, false);
    assert.equal(entry.settled, false);
    owner.close();
    await ignoredRejection;
});
test('async request rejects a completion whose known token has a different id', async () => {
    const owner = new CompletionOwner(null);
    const entry = owner.register('request', false);
    owner.publish(entry, 601n);
    owner.capture(requestCompletion(602n, entry.token));
    await assert.rejects(entry.promise, (error) => error instanceof RequestError
        && error.result === RequestResult.InternalError);
    assert.equal(entry.settled, true);
    owner.close();
});
test('zero-context synchronous request completion still correlates by id', async () => {
    const owner = new CompletionOwner(null);
    const entry = owner.register('request', false, false);
    owner.publish(entry, 701n);
    owner.capture(requestCompletion(701n, 0n));
    await entry.promise;
    assert.equal(entry.captured, true);
    assert.equal(entry.settled, true);
    owner.close();
});
test('writable token opens a retry state and does not settle the send', async () => {
    const entry = new CompletionEntry(42n, 'send');
    entry.awaitWritable(78n);
    assert.equal(entry.completionId, 78n);
    assert.equal(entry.published, true);
    assert.equal(entry.captured, false);
    assert.equal(entry.settled, false);
    entry.succeed(undefined);
    await entry.promise;
    assert.equal(entry.settled, true);
});
test('request completion registry still joins capture-before-publish', async () => {
    const entry = new CompletionEntry(43n, 'request');
    entry.capture({
        kind: 2,
        completionId: 79n,
        userContext: 43n,
        peerRoutingId: null,
        sendResult: 0,
        terminalErrno: 0,
        requestResult: 0,
        parts: [],
    });
    assert.equal(entry.captured, true);
    assert.equal(entry.settled, false);
    entry.publish(79n);
    await entry.promise;
    assert.equal(entry.settled, true);
});
test('request completion registry still joins publish-before-capture', async () => {
    const entry = new CompletionEntry(44n, 'request');
    entry.publish(80n);
    assert.equal(entry.published, true);
    assert.equal(entry.settled, false);
    entry.capture({
        kind: 2,
        completionId: 80n,
        userContext: 44n,
        peerRoutingId: null,
        sendResult: 0,
        terminalErrno: 0,
        requestResult: 0,
        parts: [],
    });
    await entry.promise;
    assert.equal(entry.settled, true);
});
