"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const zlink = require('@zlink-systems/zlink');
for (const transport of ['inproc', 'tcp']) {
    (0, node_test_1.default)(`one socket preserves multipart send and completion order (${transport})`, async () => {
        const ctx = zlink.createContext();
        const router = zlink.createRouterSocket(ctx);
        const dealer = zlink.createDealerSocket(ctx);
        const received = new zlink.Received();
        router.bind(transport === 'tcp' ? 'tcp://127.0.0.1:*' : 'inproc://completion-order');
        dealer.connect(router.options.lastEndpoint);
        try {
            // Complete the handshake before submitting a burst on this socket.
            await dealer.send().message('ready').submit();
            strict_1.default.equal(router.recv(received), true);
            received.close();
            const expected = Array.from({ length: 128 }, (_, index) => index);
            const completed = [];
            const pending = expected.map(index => dealer.send()
                .message(Buffer.from(String(index)))
                .message(Buffer.from(`payload-${index}`))
                .submit().then(() => { completed.push(index); }));
            for (const index of expected) {
                strict_1.default.equal(router.recv(received), true);
                strict_1.default.deepEqual(received.parts.map(part => part.getString()), [String(index), `payload-${index}`]);
                received.close();
            }
            await Promise.all(pending);
            strict_1.default.deepEqual(completed, expected);
            // Queue replies in request submission order and observe the runtime's
            // completion callback delivery without adding a public poller owner.
            completed.length = 0;
            const requests = expected.map(index => dealer.request()
                .message(String(index)).timeout(1000).submit().then(parts => {
                try {
                    strict_1.default.equal(parts[0].getString(), String(index));
                    completed.push(index);
                }
                finally {
                    parts.forEach(part => part.close());
                }
            }));
            const replies = [];
            for (const index of expected) {
                strict_1.default.equal(router.recv(received), true);
                strict_1.default.equal(received.parts[0].getString(), String(index));
                replies.push(received.reply().message(String(index)).submit());
                received.close();
            }
            await Promise.all([...requests, ...replies]);
            strict_1.default.deepEqual(completed, expected);
        }
        finally {
            received.close();
            dealer.close();
            router.close();
            ctx.close();
        }
    });
}
