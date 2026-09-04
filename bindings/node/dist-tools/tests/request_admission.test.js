// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
let sequence = 0;
function endpoint(label) {
    return `inproc://node-request-admission-${label}-${process.pid}-${++sequence}`;
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
function configureSmallHwm(context, dealer, router) {
    context.options.autoHwmEnabled = false;
    dealer.options.linger = 0;
    router.options.linger = 0;
    dealer.options.immediate = true;
    dealer.options.sendHwm = 512n;
    router.options.recvHwm = 512n;
}
function recvAndReplyAvailable(router, received) {
    let count = 0;
    for (;;) {
        const request = new zlink.Received();
        try {
            if (!router.recv(request, zlink.RecvFlags.DontWait))
                return count;
            assert.ok(request.replyToken instanceof zlink.ReplyToken);
            const value = request.singlePartOrThrow().getString();
            received.push(value);
            request.reply().message(`reply:${value}`).submit();
            count += 1;
        }
        finally {
            request.close();
        }
    }
}
async function hwmRequestRound(round) {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    const address = endpoint(`hwm-${round}`);
    const requestCount = 64;
    const received = [];
    configureSmallHwm(context, dealer, router);
    router.bind(address);
    dealer.connect(address);
    poller.add(dealer, [
        zlink.PollEventFlag.PollOut,
        zlink.PollEventFlag.PollCompletion,
    ], 91);
    try {
        dealer.send().message('ready').submit_sync();
        const ready = new zlink.Received();
        assert.equal(router.recv(ready), true);
        ready.close();
        const payloads = Array.from({ length: requestCount }, (_, index) => {
            const value = `${round}:${index.toString().padStart(3, '0')}:${'q'.repeat(48)}`;
            return Buffer.from(value);
        });
        const pending = payloads.map((payload) => dealer.request().message(payload).timeout(5_000).submit());
        for (const payload of payloads)
            payload.fill(0x7a);
        const initiallyAdmitted = recvAndReplyAvailable(router, received);
        assert.ok(initiallyAdmitted > 0, 'HWM setup must admit an initial request prefix');
        assert.ok(initiallyAdmitted < requestCount, 'HWM setup must leave requests waiting on WRITABLE tokens');
        while (received.length < requestCount) {
            const readyCount = poller.wait(events, 1_000);
            assert.ok(readyCount > 0, 'reply or WRITABLE progress must wake the public poller');
            assert.equal(events.slot(0), 91);
            recvAndReplyAvailable(router, received);
        }
        while (poller.wait(events, 0) > 0) { /* drain reply completions */ }
        const replies = await Promise.all(pending);
        assert.equal(new Set(received).size, requestCount, 'each submit-time request snapshot must be admitted exactly once');
        for (let index = 0; index < replies.length; index += 1) {
            const value = `${round}:${index.toString().padStart(3, '0')}:${'q'.repeat(48)}`;
            assert.equal(replies[index][0].getString(), `reply:${value}`);
            replies[index][0].close();
        }
    }
    finally {
        events.close();
        poller.close();
        closeAll(context, dealer, router);
    }
}
test('REQUEST backpressure waits for its WRITABLE token and resubmits exactly once (5 rounds)', async () => {
    for (let round = 0; round < 5; round += 1)
        await hwmRequestRound(round);
});
test('connect-before-bind REQUEST resumes from WRITABLE and then awaits its reply', async () => {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    const address = endpoint('connect-before-bind');
    dealer.options.immediate = true;
    dealer.connect(address);
    poller.add(dealer, [
        zlink.PollEventFlag.PollOut,
        zlink.PollEventFlag.PollCompletion,
    ], 92);
    try {
        const pending = dealer.request().message('before-bind').timeout(5_000).submit();
        router.bind(address);
        assert.ok(poller.wait(events, 1_000) > 0, 'binding the peer must publish the request wait token as WRITABLE');
        const request = new zlink.Received();
        assert.equal(router.recv(request), true);
        assert.equal(request.singlePartOrThrow().getString(), 'before-bind');
        request.reply().message('after-bind').submit();
        assert.ok(poller.wait(events, 1_000) > 0);
        const reply = await pending;
        assert.equal(reply[0].getString(), 'after-bind');
        reply[0].close();
        request.close();
    }
    finally {
        events.close();
        poller.close();
        closeAll(context, dealer, router);
    }
});
test('socket close releases a connect-before-bind REQUEST token', async () => {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    dealer.options.immediate = true;
    dealer.connect(endpoint('close-token'));
    const pending = dealer.request().message('close-me').timeout(30_000).submit();
    const rejection = assert.rejects(pending, (error) => error instanceof zlink.RequestError
        && error.result === zlink.RequestResult.Terminated);
    dealer.close();
    try {
        await rejection;
    }
    finally {
        context.close();
    }
});
test('context shutdown terminates a REQUEST wait token as typed Terminated', async () => {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const address = endpoint('shutdown-token');
    configureSmallHwm(context, dealer, router);
    router.bind(address);
    dealer.connect(address);
    try {
        dealer.send().message('ready').submit_sync();
        const ready = new zlink.Received();
        assert.equal(router.recv(ready), true);
        ready.close();
        const outcomes = [];
        const pending = Array.from({ length: 64 }, (_, index) => dealer.request().message(Buffer.alloc(64, index)).timeout(30_000).submit().then(() => outcomes.push(null), (error) => outcomes.push(error)));
        context.shutdown();
        for (let turn = 0; turn < 1_000
            && !outcomes.some((error) => error instanceof zlink.SubmitError); turn += 1) {
            await new Promise((resolve) => setImmediate(resolve));
        }
        assert.ok(outcomes.some((error) => error instanceof zlink.SubmitError
            && error.result === zlink.SubmitResult.Terminated), 'at least one pre-admission wait token must terminate as SubmitError.Terminated');
        dealer.close();
        await Promise.all(pending);
    }
    finally {
        router.close();
        dealer.close();
        context.close();
    }
});
test('SEND and REQUEST WRITABLE tokens share one completion owner', async () => {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    const address = endpoint('mixed');
    const operationCount = 64;
    let dataCount = 0;
    let requestCount = 0;
    configureSmallHwm(context, dealer, router);
    router.bind(address);
    dealer.connect(address);
    poller.add(dealer, [
        zlink.PollEventFlag.PollOut,
        zlink.PollEventFlag.PollCompletion,
    ], 93);
    try {
        dealer.send().message('ready').submit_sync();
        const ready = new zlink.Received();
        assert.equal(router.recv(ready), true);
        ready.close();
        const sends = [];
        const requests = [];
        for (let index = 0; index < operationCount; index += 1) {
            const value = `${index.toString().padStart(3, '0')}:${'m'.repeat(52)}`;
            if ((index & 1) === 0)
                sends.push(dealer.send().message(value).submit());
            else
                requests.push(dealer.request().message(value).timeout(5_000).submit());
        }
        while (dataCount + requestCount < operationCount) {
            for (;;) {
                const received = new zlink.Received();
                try {
                    if (!router.recv(received, zlink.RecvFlags.DontWait))
                        break;
                    if (received.replyToken instanceof zlink.ReplyToken) {
                        received.reply().message('mixed-reply').submit();
                        requestCount += 1;
                    }
                    else {
                        dataCount += 1;
                    }
                }
                finally {
                    received.close();
                }
            }
            if (dataCount + requestCount < operationCount) {
                assert.ok(poller.wait(events, 1_000) > 0, 'mixed WRITABLE records must make progress through one drain owner');
            }
        }
        while (poller.wait(events, 0) > 0) { /* drain final request completions */ }
        await Promise.all(sends);
        const replies = await Promise.all(requests);
        assert.equal(dataCount, operationCount / 2);
        assert.equal(requestCount, operationCount / 2);
        for (const reply of replies) {
            assert.equal(reply[0].getString(), 'mixed-reply');
            reply[0].close();
        }
    }
    finally {
        events.close();
        poller.close();
        closeAll(context, dealer, router);
    }
});
