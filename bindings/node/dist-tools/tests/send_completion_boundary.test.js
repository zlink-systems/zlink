// SPDX-License-Identifier: MPL-2.0
'use strict';
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_path_1 = __importDefault(require("node:path"));
const submitObservations = [];
const callbackCompletionKeys = [];
const addonPath = node_path_1.default.resolve(__dirname, '../../build/Release/zlink.node');
const rawNative = require(addonPath);
const rawSendAsync = rawNative.socketSendAsync;
const rawCompletionHandler = rawNative.socketSendCompletionHandler;
const observedNative = new Proxy({}, {
    get(_target, property) {
        if (property === 'socketSendAsync') {
            return (...args) => {
                const result = Reflect.apply(rawSendAsync, rawNative, args);
                submitObservations.push({
                    keys: Object.keys(result).sort(),
                    inlineCompletionKeys: result.inlineCompletion
                        ? Object.keys(result.inlineCompletion).sort()
                        : undefined
                });
                return result;
            };
        }
        if (property === 'socketSendCompletionHandler') {
            return (socket, handler) => Reflect.apply(rawCompletionHandler, rawNative, [
                socket,
                (event) => {
                    callbackCompletionKeys.push(Object.keys(event).sort());
                    handler(event);
                }
            ]);
        }
        return Reflect.get(rawNative, property);
    }
});
const addonModule = require.cache[require.resolve(addonPath)];
if (!addonModule)
    throw new Error('native addon cache entry is missing');
addonModule.exports = observedNative;
const zlink = require('@zlink-systems/zlink');
function endpoint(label) {
    return `inproc://node-send-completion-boundary-${label}-${process.pid}`;
}
function nextTurn() {
    return new Promise((resolve) => setImmediate(resolve));
}
function within(promise, timeoutMs = 1_000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`operation did not settle within ${timeoutMs}ms`)), timeoutMs);
        promise.then((value) => { clearTimeout(timer); resolve(value); }, (error) => { clearTimeout(timer); reject(error); });
    });
}
function closeAll(context, ...sockets) {
    for (const socket of sockets.reverse()) {
        try {
            socket.close();
        }
        catch { /* cleanup should not mask the assertion */ }
    }
    context.close();
}
function assertSubmitShape(observation) {
    const expected = observation.inlineCompletionKeys
        ? ['inlineCompletion', 'nativeErrno', 'result']
        : ['nativeErrno', 'result'];
    strict_1.default.deepEqual(observation.keys, expected);
}
const completionShape = ['result', 'terminalErrno', 'token'];
(0, node_test_1.default)('native send completion boundary exposes only Promise terminal fields', async () => {
    const directContext = zlink.createContext();
    const directSender = zlink.createPairSocket(directContext);
    const directReceiver = zlink.createPairSocket(directContext);
    try {
        directSender.bind(endpoint('direct'));
        directReceiver.connect(directSender.options.lastEndpoint);
        submitObservations.length = 0;
        callbackCompletionKeys.length = 0;
        await directSender.send().message('direct').submit();
        strict_1.default.equal(submitObservations.length, 1);
        assertSubmitShape(submitObservations[0]);
        const directCompletion = submitObservations[0].inlineCompletionKeys
            ?? callbackCompletionKeys[0];
        strict_1.default.deepEqual(directCompletion, completionShape);
    }
    finally {
        closeAll(directContext, directSender, directReceiver);
    }
    const deferredContext = zlink.createContext();
    deferredContext.options.autoHwmEnabled = false;
    const deferredSender = zlink.createPairSocket(deferredContext);
    const deferredReceiver = zlink.createPairSocket(deferredContext);
    deferredSender.options.sendHwm = 4096n;
    deferredReceiver.options.recvHwm = 4096n;
    const first = zlink.Message.from(Buffer.alloc(4_096, 0x61));
    const pending = zlink.Message.from(Buffer.alloc(4_096, 0x62));
    try {
        deferredSender.bind(endpoint('deferred'));
        deferredReceiver.connect(deferredSender.options.lastEndpoint);
        await deferredSender.send().message(first).submit();
        submitObservations.length = 0;
        callbackCompletionKeys.length = 0;
        const terminal = deferredSender.send().message(pending).timeout(-1).submit();
        await nextTurn();
        strict_1.default.equal(submitObservations.length, 1);
        strict_1.default.equal(submitObservations[0].inlineCompletionKeys, undefined);
        assertSubmitShape(submitObservations[0]);
        deferredSender.close();
        await strict_1.default.rejects(within(terminal), (error) => error instanceof zlink.SubmitError
            && error.result === zlink.SubmitResult.Terminated);
        strict_1.default.deepEqual(callbackCompletionKeys, [completionShape]);
    }
    finally {
        closeAll(deferredContext, deferredSender, deferredReceiver);
        first.close();
        pending.close();
    }
});
