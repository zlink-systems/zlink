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
(0, node_test_1.default)('context close calls shutdown before term', () => {
    const context = zlink.createContext();
    const calls = [];
    const original = nativeModule.requireNative;
    const native = original();
    nativeModule.requireNative = () => new Proxy({}, {
        get(_target, property) {
            if (property === 'ctxShutdown' || property === 'ctxTerm') {
                return (...args) => {
                    calls.push(String(property));
                    return native[property](...args);
                };
            }
            return native[property];
        }
    });
    try {
        context.close();
        strict_1.default.deepEqual(calls, ['ctxShutdown', 'ctxTerm']);
    }
    finally {
        nativeModule.requireNative = original;
        context.close();
    }
});
(0, node_test_1.default)('busy poller destroy reports typed Busy and leaves poller valid', () => {
    const poller = zlink.createPoller();
    const original = nativeModule.requireNative();
    let busy = true;
    const restore = intercept('pollerDestroy', (...args) => {
        if (busy) {
            busy = false;
            throw Object.assign(new Error('poller busy'), { nativeErrno: 16 });
        }
        return original.pollerDestroy(...args);
    });
    try {
        strict_1.default.throws(() => poller.close(), (error) => error instanceof zlink.CloseError && error.result === zlink.CloseResult.Busy);
        strict_1.default.equal(poller.size, 0);
        poller.close();
    }
    finally {
        restore();
        poller.close();
    }
});
