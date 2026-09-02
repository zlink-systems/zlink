// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const { CompletionEntry } = require('../../dist/zlink/runtime/messaging/completion_owner');
test('provisional completion registry joins capture-before-publish', async () => {
    const entry = new CompletionEntry(41n, 'send');
    entry.capture({
        kind: 1,
        completionId: 77n,
        userContext: 41n,
        sendResult: 0,
        terminalErrno: 0,
        requestResult: 0,
    });
    assert.equal(entry.captured, true);
    assert.equal(entry.settled, false);
    entry.publish(77n);
    await entry.promise;
    assert.equal(entry.settled, true);
});
test('provisional completion registry joins publish-before-capture', async () => {
    const entry = new CompletionEntry(42n, 'send');
    entry.publish(78n);
    assert.equal(entry.published, true);
    assert.equal(entry.settled, false);
    entry.capture({
        kind: 1,
        completionId: 78n,
        userContext: 42n,
        sendResult: 0,
        terminalErrno: 0,
        requestResult: 0,
    });
    await entry.promise;
    assert.equal(entry.settled, true);
});
