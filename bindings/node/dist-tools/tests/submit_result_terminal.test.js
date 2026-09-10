// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
let sequence = 0;
function endpoint(label) {
    return `inproc://node-submit-result-${label}-${process.pid}-${++sequence}`;
}
function closeAll(context, ...items) {
    for (const item of items.reverse()) {
        try {
            item.close();
        }
        catch { /* preserve the assertion */ }
    }
    context.close();
}
async function isFulfilledNow(stage) {
    let fulfilled = false;
    void stage.then(() => { fulfilled = true; }, () => { });
    await Promise.resolve();
    return fulfilled;
}
test('submit result reports immediate SEND and REQUEST admission', async () => {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const address = endpoint('immediate');
    router.bind(address);
    dealer.connect(address);
    try {
        dealer.send().message('ready').submit_sync();
        const received = new zlink.Received();
        assert.equal(router.recv(received), true);
        received.close();
        const sent = dealer.send().message('send').submit();
        assert.equal(sent.result, zlink.SubmitResult.Ok);
        assert.equal(await isFulfilledNow(sent.admitted), true);
        assert.equal(router.recv(received), true);
        assert.equal(received.singlePartOrThrow().getString(), 'send');
        received.close();
        const requested = dealer.request().message('request').timeout(1_000).submit();
        assert.equal(requested.result, zlink.SubmitResult.Ok);
        assert.equal(await isFulfilledNow(requested.admitted), true);
        assert.equal(await isFulfilledNow(requested.reply), false);
        assert.equal(router.recv(received), true);
        assert.equal(received.singlePartOrThrow().getString(), 'request');
        received.reply().message('reply').submit();
        received.close();
        const reply = await requested.reply;
        try {
            assert.equal(reply[0].getString(), 'reply');
        }
        finally {
            for (const part of reply)
                part.close();
        }
    }
    finally {
        closeAll(context, dealer, router);
    }
});
test('BACKPRESSURED admission completes after public WRITABLE progress', async () => {
    const context = zlink.createContext();
    context.options.autoHwmEnabled = false;
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    const address = endpoint('send-backpressure');
    sender.options.immediate = true;
    sender.options.sendHwm = 512n;
    receiver.options.recvHwm = 512n;
    receiver.bind(address);
    sender.connect(address);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    poller.add(sender, [
        zlink.PollEventFlag.PollOut,
        zlink.PollEventFlag.PollCompletion,
    ], 41);
    try {
        sender.send().message('ready').submit_sync();
        const received = new zlink.Received();
        assert.equal(receiver.recv(received), true);
        received.close();
        let blocked = null;
        let submitted = 0;
        for (; submitted < 512; submitted += 1) {
            const submission = sender.send().message(Buffer.alloc(64, submitted)).submit();
            if (submission.result === zlink.SubmitResult.Backpressured) {
                blocked = submission;
                break;
            }
            assert.equal(submission.result, zlink.SubmitResult.Ok);
            assert.equal(await isFulfilledNow(submission.admitted), true);
        }
        assert.ok(blocked, 'small HWM must return BACKPRESSURED');
        assert.equal(await isFulfilledNow(blocked.admitted), false);
        for (let index = 0; index < submitted; index += 1) {
            assert.equal(receiver.recv(received), true);
            received.close();
        }
        assert.equal(poller.wait(events, 5_000), 1);
        assert.equal(events.slot(0), 41);
        assert.equal(events.hasEvent(0, zlink.PollEventFlag.PollOut), true);
        await blocked.admitted;
        assert.equal(receiver.recv(received), true);
        received.close();
    }
    finally {
        events.close();
        poller.close();
        closeAll(context, sender, receiver);
    }
    const requestContext = zlink.createContext();
    requestContext.options.autoHwmEnabled = false;
    const dealer = zlink.createDealerSocket(requestContext);
    const router = zlink.createRouterSocket(requestContext);
    const requestAddress = endpoint('request-backpressure');
    dealer.options.immediate = true;
    dealer.options.sendHwm = 512n;
    router.options.recvHwm = 512n;
    router.bind(requestAddress);
    dealer.connect(requestAddress);
    const requestPoller = zlink.createPoller();
    const requestEvents = zlink.createPollEvents(1);
    requestPoller.add(dealer, [
        zlink.PollEventFlag.PollOut,
        zlink.PollEventFlag.PollCompletion,
    ], 42);
    try {
        dealer.send().message('ready').submit_sync();
        const received = new zlink.Received();
        assert.equal(router.recv(received), true);
        received.close();
        const submissions = [];
        let blocked = null;
        let blockedValue = '';
        for (let index = 0; index < 512; index += 1) {
            const value = `request-${index}:${'x'.repeat(48)}`;
            const submission = dealer.request().message(value).timeout(5_000).submit();
            submissions.push(submission);
            if (submission.result === zlink.SubmitResult.Backpressured) {
                blocked = submission;
                blockedValue = value;
                break;
            }
            assert.equal(submission.result, zlink.SubmitResult.Ok);
        }
        assert.ok(blocked, 'small request HWM must return BACKPRESSURED');
        assert.equal(await isFulfilledNow(blocked.admitted), false);
        assert.equal(await isFulfilledNow(blocked.reply), false);
        for (;;) {
            const request = new zlink.Received();
            try {
                if (!router.recv(request, zlink.RecvFlags.DontWait))
                    break;
                assert.notEqual(request.singlePartOrThrow().getString(), blockedValue);
                request.reply().message('prefix-reply').submit();
            }
            finally {
                request.close();
            }
        }
        while (!await isFulfilledNow(blocked.admitted)) {
            assert.equal(requestPoller.wait(requestEvents, 5_000), 1);
            assert.equal(requestEvents.slot(0), 42);
        }
        assert.equal(await isFulfilledNow(blocked.reply), false, 'reply cannot complete at admission');
        const blockedRequest = new zlink.Received();
        assert.equal(router.recv(blockedRequest), true);
        assert.equal(blockedRequest.singlePartOrThrow().getString(), blockedValue);
        blockedRequest.reply().message('blocked-reply').submit();
        blockedRequest.close();
        while (!await isFulfilledNow(blocked.reply)) {
            assert.equal(requestPoller.wait(requestEvents, 5_000), 1);
            assert.equal(requestEvents.slot(0), 42);
        }
        const blockedReply = await blocked.reply;
        try {
            assert.equal(blockedReply[0].getString(), 'blocked-reply');
        }
        finally {
            for (const part of blockedReply)
                part.close();
        }
        const prefixReplies = await Promise.all(submissions.slice(0, -1).map((submission) => submission.reply));
        for (const reply of prefixReplies) {
            assert.equal(reply[0].getString(), 'prefix-reply');
            for (const part of reply)
                part.close();
        }
    }
    finally {
        requestEvents.close();
        requestPoller.close();
        closeAll(requestContext, dealer, router);
    }
});
