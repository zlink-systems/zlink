// SPDX-License-Identifier: MPL-2.0
'use strict';
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_path_1 = __importDefault(require("node:path"));
const completion_poller_1 = require("./completion_poller");
const zlink = require('@zlink-systems/zlink');
const nativeModule = require(node_path_1.default.resolve(__dirname, '../../dist/zlink/runtime/native/native.js'));
let sequence = 0;
function endpoint(label) {
    return `inproc://node-core-parity-${label}-${process.pid}-${++sequence}`;
}
function intercept(method, replacement) {
    const original = nativeModule.requireNative;
    const native = original();
    nativeModule.requireNative = () => new Proxy({}, {
        get(_target, property) {
            return property === method ? replacement : native[property];
        }
    });
    return () => { nativeModule.requireNative = original; };
}
(0, node_test_1.default)('socket close reports Busy at once and leaves the socket open', async () => {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const address = endpoint('close-busy');
    router.bind(address);
    dealer.connect(address);
    const original = nativeModule.requireNative();
    let calls = 0;
    const restore = intercept('socketClose', (...args) => {
        calls += 1;
        if (calls === 1) {
            throw Object.assign(new Error('socket busy'), {
                nativeErrno: 16,
                nativeResult: zlink.CloseResult.Busy,
            });
        }
        return original.socketClose(...args);
    });
    const driver = new completion_poller_1.CompletionPollerDriver(context, dealer);
    try {
        const pending = dealer.request().message('ping').timeout(5_000).submit();
        strict_1.default.throws(() => dealer.close(), (error) => error instanceof zlink.CloseError && error.result === zlink.CloseResult.Busy);
        strict_1.default.equal(calls, 1, 'close must not retry or wait after Busy');
        // The refused close leaves the request in flight and the socket usable.
        const request = new zlink.Received();
        strict_1.default.equal(router.recv(request), true);
        request.reply().message('pong').submit();
        request.close();
        const reply = await driver.settle(pending.reply);
        strict_1.default.equal(reply[0].getString(), 'pong');
        dealer.close();
        strict_1.default.equal(calls, 2);
    }
    finally {
        restore();
        driver.close();
        try {
            dealer.close();
        }
        catch { /* preserve the assertion */ }
        router.close();
        context.close();
    }
});
for (const kind of ['dealer', 'router']) {
    (0, node_test_1.default)(`${kind} request without a timeout passes 0 so Core applies the socket option`, async () => {
        const original = nativeModule.requireNative();
        const timeouts = [];
        // A socket captures the native module when it is created.
        const restore = intercept('socketSubmitRequest', (...args) => {
            timeouts.push(args[3]);
            return original.socketSubmitRequest(...args);
        });
        const context = zlink.createContext();
        const requester = kind === 'dealer'
            ? zlink.createDealerSocket(context)
            : zlink.createRouterSocket(context);
        const peer = zlink.createRouterSocket(context);
        const driver = new completion_poller_1.CompletionPollerDriver(context, requester);
        try {
            const address = endpoint(`timeout-${kind}`);
            requester.options.requestTimeout = 300;
            if (kind === 'router') {
                requester.setRoutingId(zlink.RoutingId.from('requester'));
                peer.setRoutingId(zlink.RoutingId.from('peer'));
            }
            peer.bind(address);
            requester.connect(address);
            // The peer never replies, so the socket option decides the deadline.
            const started = Date.now();
            const submission = kind === 'dealer'
                ? requester.request().message('q').submit()
                : requester.request(zlink.RoutingId.from('peer')).message('q').submit();
            await strict_1.default.rejects(driver.settle(submission.reply, 4_000), (error) => error instanceof zlink.RequestError && error.result === zlink.RequestResult.TimedOut);
            strict_1.default.ok(Date.now() - started < 3_000, 'the socket option, not a binding default, ends the request');
            strict_1.default.deepEqual(timeouts, [0]);
        }
        finally {
            restore();
            driver.close();
            requester.close();
            peer.close();
            context.close();
        }
    });
}
