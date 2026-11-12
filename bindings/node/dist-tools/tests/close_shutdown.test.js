"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_path_1 = __importDefault(require("node:path"));
const zlink = require('@zlink-systems/zlink');
const nativeModule = require(node_path_1.default.resolve(__dirname, '../../dist/zlink/runtime/native/native.js'));
const handles = require(node_path_1.default.resolve(__dirname, '../../dist/zlink/runtime/handles/native_handle.js'));
function settledWithin(promise, ms) {
    return new Promise(resolve => {
        const timer = setTimeout(() => resolve('pending'), ms);
        promise.then(() => { clearTimeout(timer); resolve('resolved'); }, (error) => { clearTimeout(timer); resolve(error); });
    });
}
// Core returns SHUTDOWN when a close of the same handle was already accepted.
// The socket is finished then: pending work ends before the error is reported.
(0, node_test_1.default)('socket close reported as SHUTDOWN settles pending requests before throwing', async () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    const received = new zlink.Received();
    const poller = zlink.createPoller();
    router.bind('inproc://close-shutdown');
    dealer.connect('inproc://close-shutdown');
    poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 1);
    try {
        const pending = dealer.request().message('pending').timeout(5000).submit().reply;
        strict_1.default.equal(router.recv(received), true);
        received.close();
        poller.remove(dealer);
        nativeModule.requireNative().socketClose(handles.getNativeHandle(dealer));
        strict_1.default.throws(() => dealer.close(), (error) => error instanceof zlink.CloseError && error.result === zlink.CloseResult.Shutdown);
        const outcome = await settledWithin(pending, 1000);
        strict_1.default.ok(outcome instanceof zlink.RequestError, String(outcome));
        strict_1.default.equal(outcome.result, zlink.RequestResult.Terminated);
        dealer.close();
    }
    finally {
        poller.close();
        received.close();
        try {
            dealer.close();
        }
        catch { /* reported by the assertions above */ }
        router.close();
        ctx.close();
    }
});
