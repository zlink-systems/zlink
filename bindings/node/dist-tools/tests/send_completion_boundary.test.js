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
                await new Promise((resolve) => setTimeout(resolve, 1));
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
