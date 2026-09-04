// SPDX-License-Identifier: MPL-2.0
'use strict';
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_path_1 = __importDefault(require("node:path"));
const addonPath = node_path_1.default.resolve(__dirname, '../../build/Release/zlink.node');
const native = require(addonPath);
const zlink = require('@zlink-systems/zlink');
let sequence = 0;
function endpoint(label) {
    return `inproc://node-completion-boundary-${label}-${process.pid}-${++sequence}`;
}
(0, node_test_1.default)('native completion recv closes an unclaimed late completion exactly once', async () => {
    native.testCompletionCloseCount(true);
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    router.bind('inproc://late-completion-cleanup');
    dealer.connect('inproc://late-completion-cleanup');
    try {
        const submitted = native.socketSubmitRequest(dealer._native, null, Buffer.from('late'), 1_000, zlink.SendFlags.DontWait, 999n);
        strict_1.default.equal(submitted.result, zlink.SubmitResult.Ok);
        strict_1.default.notEqual(submitted.completionId, 0n);
        const request = new zlink.Received();
        strict_1.default.equal(router.recv(request), true);
        request.reply().message('ignored').submit();
        let completion = null;
        for (let attempt = 0; attempt < 100 && !completion; attempt += 1) {
            completion = native.socketCompletionRecv(dealer._native, zlink.RecvFlags.DontWait);
            if (!completion)
                await new Promise((resolve) => setImmediate(resolve));
        }
        strict_1.default.ok(completion);
        strict_1.default.equal(completion.userContext, 999n);
        strict_1.default.equal(native.testCompletionCloseCount(false), 1n);
        strict_1.default.equal(native.socketCompletionRecv(dealer._native, zlink.RecvFlags.DontWait), null);
        strict_1.default.equal(native.testCompletionCloseCount(false), 1n);
        request.close();
    }
    finally {
        dealer.close();
        router.close();
        context.close();
    }
});
(0, node_test_1.default)('completion boundary exposes pull records without callback functions', () => {
    for (const name of [
        'socketSubmitSend',
        'socketSubmitRequest',
        'socketRequestSync',
        'socketCompletionRecv',
    ])
        strict_1.default.equal(typeof native[name], 'function', name);
    strict_1.default.equal(native.socketSendCompletionHandler, undefined);
    strict_1.default.equal(native.socketRequestCompletionHandler, undefined);
});
(0, node_test_1.default)('DONTWAIT routed HWM refusal preserves RID through writable-token retry', () => {
    const context = zlink.createContext();
    context.options.autoHwmEnabled = false;
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    const peer = zlink.RoutingId.from('node-native-writable-peer');
    const peerBytes = peer.toBytes();
    const userContext = 0xb79n;
    let blockedPayload = null;
    let waitToken = 0n;
    let accepted = 0;
    try {
        router.options.linger = 0;
        dealer.options.linger = 0;
        router.options.immediate = true;
        router.options.mandatory = true;
        router.options.sendHwm = 512n;
        dealer.options.recvHwm = 512n;
        dealer.setRoutingId(peer);
        const address = endpoint('writable');
        router.bind(address);
        dealer.connect(address);
        dealer.send().message('route-ready').submit_sync();
        const routeReady = new zlink.Received();
        strict_1.default.equal(router.recv(routeReady), true);
        strict_1.default.ok(routeReady.routingId?.equals(peer));
        routeReady.close();
        // POLLOUT is a WRITABLE-token wake, not a connection-ready signal.
        poller.add(router, [zlink.PollEventFlag.PollOut], 79);
        strict_1.default.equal(poller.wait(events, 0), 0);
        for (; accepted < 512; accepted += 1) {
            const payload = Buffer.alloc(64, 0x68);
            payload.writeUInt32LE(accepted, 0);
            const submitted = native.socketSubmitSend(router._native, payload, peerBytes, zlink.SendFlags.DontWait, userContext);
            if (submitted.result === zlink.SubmitResult.Backpressured) {
                strict_1.default.equal(submitted.nativeErrno, 11);
                strict_1.default.notEqual(submitted.completionId, 0n);
                blockedPayload = payload;
                waitToken = submitted.completionId;
                break;
            }
            strict_1.default.equal(submitted.result, zlink.SubmitResult.Ok);
            strict_1.default.equal(submitted.nativeErrno, 0);
            strict_1.default.equal(submitted.completionId, 0n);
        }
        strict_1.default.ok(accepted > 0 && accepted < 512, 'routed DONTWAIT send must reach physical HWM');
        strict_1.default.ok(blockedPayload);
        strict_1.default.equal(native.socketCompletionRecv(router._native, zlink.RecvFlags.DontWait), null);
        strict_1.default.equal(poller.wait(events, 0), 0, 'the HWM-full socket must not remain writable');
        for (let index = 0; index < accepted; index += 1) {
            const received = new zlink.Received();
            strict_1.default.equal(dealer.recv(received), true);
            received.close();
        }
        strict_1.default.equal(poller.wait(events, 5_000), 1);
        strict_1.default.equal(events.slot(0), 79);
        strict_1.default.equal(events.hasEvent(0, zlink.PollEventFlag.PollOut), true);
        const writable = native.socketCompletionRecv(router._native, zlink.RecvFlags.DontWait);
        strict_1.default.ok(writable);
        strict_1.default.equal(writable.kind, zlink.CompletionKind.Writable);
        strict_1.default.equal(writable.completionId, waitToken);
        strict_1.default.equal(writable.userContext, userContext);
        strict_1.default.deepEqual(writable.peerRoutingId, peerBytes);
        strict_1.default.equal(writable.sendResult, zlink.SubmitResult.Ok);
        strict_1.default.equal(writable.terminalErrno, 0);
        strict_1.default.equal(native.socketCompletionRecv(router._native, zlink.RecvFlags.DontWait), null);
        const retried = native.socketSubmitSend(router._native, blockedPayload, peerBytes, zlink.SendFlags.DontWait, userContext);
        strict_1.default.equal(retried.result, zlink.SubmitResult.Ok);
        strict_1.default.equal(retried.nativeErrno, 0);
        strict_1.default.equal(retried.completionId, 0n);
        const receivedRetry = new zlink.Received();
        strict_1.default.equal(dealer.recv(receivedRetry), true);
        strict_1.default.deepEqual(receivedRetry.singlePartOrThrow().data(), blockedPayload);
        receivedRetry.close();
        const duplicate = new zlink.Received();
        strict_1.default.equal(dealer.recv(duplicate, zlink.RecvFlags.DontWait), false, 'the rejected attempt must not have retained or delivered the payload');
        duplicate.close();
        strict_1.default.equal(native.socketCompletionRecv(router._native, zlink.RecvFlags.DontWait), null, 'ordinary successful SEND must not publish a completion');
    }
    finally {
        events.close();
        poller.close();
        dealer.close();
        router.close();
        context.close();
    }
});
