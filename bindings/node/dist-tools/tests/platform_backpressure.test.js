"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_fs_1 = __importDefault(require("node:fs"));
const node_module_1 = require("node:module");
const node_vm_1 = __importDefault(require("node:vm"));
const nativeErrorsPath = require.resolve('../../dist/zlink/runtime/errors/native_errors');
const nativeErrors = require(nativeErrorsPath);
const { CompletionOwner } = require('../../dist/zlink/runtime/messaging/completion_owner');
const { SubmitResult } = require('../../dist/zlink/contracts/errors/errors');
// Evaluate the real errno boundary with each OS's constants. Do not treat both
// 11 and 35 as EAGAIN: they exchange meanings with EDEADLK on Linux and Darwin.
for (const [platform, again, deadlock] of [['linux', 11, 35], ['darwin', 35, 11]]) {
    (0, node_test_1.default)(`${platform} EAGAIN preserves send/request wait tokens through repeated backpressure`, async () => {
        const localRequire = (0, node_module_1.createRequire)(nativeErrorsPath);
        const exports = {};
        node_vm_1.default.runInNewContext(node_fs_1.default.readFileSync(nativeErrorsPath, 'utf8'), {
            exports,
            require: (name) => name === 'node:os'
                ? { constants: { errno: { EAGAIN: again, EDEADLK: deadlock } } }
                : localRequire(name),
        }, { filename: nativeErrorsPath });
        strict_1.default.equal(exports.isWouldBlock(again), true);
        strict_1.default.equal(exports.isWouldBlock(deadlock), false);
        strict_1.default.equal(exports.isWouldBlock(0), false);
        const original = nativeErrors.isWouldBlock;
        nativeErrors.isWouldBlock = exports.isWouldBlock;
        try {
            for (const kind of ['send', 'request']) {
                const owner = new CompletionOwner(null);
                const publicOwner = {};
                owner.transferToPublic(publicOwner);
                const queue = [];
                let calls = 0;
                const submit = (token) => {
                    if (++calls <= 2) {
                        const completionId = BigInt(calls);
                        queue.push({ kind: 3, completionId, userContext: token,
                            sendResult: 0, terminalErrno: 0, requestResult: 0 });
                        return { result: SubmitResult.Backpressured, nativeErrno: again, completionId };
                    }
                    if (kind === 'request')
                        queue.push({ kind: 2, completionId: 3n,
                            userContext: token, requestResult: 0, parts: [] });
                    return { result: SubmitResult.Ok, nativeErrno: 0,
                        completionId: kind === 'send' ? 0n : 3n };
                };
                owner.native = {
                    socketSubmitSend: (_h, _p, _r, _f, token) => submit(token),
                    socketSubmitRequest: (_h, _r, _p, _t, _f, token) => submit(token),
                    socketCompletionRecv: () => queue.shift() ?? null,
                };
                try {
                    const pending = kind === 'send'
                        ? owner.submitSend(Buffer.from('retained'), null)
                        : owner.submitRequest(Buffer.from('retained'), null, 1000);
                    strict_1.default.equal(owner.hasManagedWritableWait(), true);
                    owner.drain(publicOwner);
                    strict_1.default.equal(owner.hasManagedWritableWait(), true, 'second refusal keeps the replacement token');
                    owner.drain(publicOwner);
                    if (kind === 'request')
                        owner.drain(publicOwner);
                    await pending;
                    strict_1.default.equal(calls, 3);
                    strict_1.default.equal(owner.hasManagedWritableWait(), false);
                    // A real missing token must still fail; platform handling must not
                    // weaken the binding's validation of the Core contract.
                    owner.native.socketSubmitSend = owner.native.socketSubmitRequest = () => ({
                        result: SubmitResult.Backpressured, nativeErrno: again, completionId: 0n,
                    });
                    await strict_1.default.rejects(kind === 'send'
                        ? owner.submitSend(Buffer.from('invalid'), null)
                        : owner.submitRequest(Buffer.from('invalid'), null, 1000), { result: SubmitResult.Backpressured, nativeErrno: again });
                }
                finally {
                    owner.close();
                }
            }
        }
        finally {
            nativeErrors.isWouldBlock = original;
        }
    });
}
