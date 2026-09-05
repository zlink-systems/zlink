"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const zlink = require('@zlink-systems/zlink');
for (const count of [2, 17]) {
    (0, node_test_1.default)(`routed ${count}-part reply preserves bytes, properties and consumption`, async () => {
        const context = zlink.createContext();
        const router = zlink.createRouterSocket(context);
        const dealer = zlink.createDealerSocket(context);
        const received = new zlink.Received();
        const poller = zlink.createPoller();
        const events = zlink.createPollEvents(2);
        const payloads = Array.from({ length: count }, (_, index) => Buffer.alloc(index === count - 1 ? 0 : index === 0 ? 65536 : index, index + 1));
        try {
            dealer.setRoutingId(zlink.RoutingId.from(Buffer.from('multipart-relay')));
            router.bind(`inproc://multipart-hot-path-${count}`);
            dealer.connect(`inproc://multipart-hot-path-${count}`);
            poller.add(router, [zlink.PollEventFlag.PollIn], 0);
            poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 1);
            let operation = dealer.request().timeout(1000);
            for (const payload of payloads)
                operation = operation.message(payload);
            const pending = operation.submit();
            // Admission may await WRITABLE. The public owner must keep progressing
            // it while the receiver waits; blocking recv alone cannot run JS retry.
            while (!router.recv(received, zlink.RecvFlags.DontWait)) {
                strict_1.default.ok(poller.wait(events, 1000) > 0);
            }
            strict_1.default.equal(received.parts.length, count);
            let reply = received.reply();
            for (let index = 0; index < count; ++index) {
                const part = received.parts[index];
                strict_1.default.equal(part.size(), payloads[index].length);
                strict_1.default.equal(part.getProperty('Routing-Id'), 'multipart-relay');
                strict_1.default.equal(part.getProperty('Identity'), 'multipart-relay');
                reply = reply.message(part);
            }
            reply.submit();
            for (const part of received.parts)
                strict_1.default.equal(part.size(), 0);
            received.close();
            strict_1.default.ok(poller.wait(events, 1000) > 0);
            const parts = await pending;
            try {
                strict_1.default.deepEqual(parts.map((part) => part.data()), payloads);
            }
            finally {
                for (const part of parts)
                    part.close();
            }
        }
        finally {
            received.close();
            events.close();
            poller.close();
            dealer.close();
            router.close();
            context.close();
        }
    });
}
(0, node_test_1.default)('observed multipart data survives receive reuse and reply consumption', async () => {
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    const received = new zlink.Received();
    try {
        router.bind('inproc://multipart-observed-data');
        dealer.connect('inproc://multipart-observed-data');
        for (let round = 0; round < 2; ++round) {
            const pending = dealer.request().message('before').message('').timeout(1000).submit();
            strict_1.default.equal(router.recv(received), true);
            const view = received.parts[0].data();
            view.write('edited');
            received.reply().message(received.parts[0]).message(received.parts[1]).submit();
            received.close();
            const parts = await pending;
            strict_1.default.equal(parts[0].getString(), 'edited');
            for (const part of parts)
                part.close();
            strict_1.default.equal(view.toString(), 'edited');
        }
    }
    finally {
        received.close();
        dealer.close();
        router.close();
        context.close();
    }
});
(0, node_test_1.default)('mixed multipart normalization preserves prefixes and input buffers', async () => {
    const context = zlink.createContext();
    const left = zlink.createPairSocket(context);
    const right = zlink.createPairSocket(context);
    const received = new zlink.Received();
    try {
        left.bind('inproc://multipart-conversion');
        right.connect('inproc://multipart-conversion');
        const prefix = Buffer.from('prefix');
        const bytes = new Uint8Array([1, 2, 3, 4]).subarray(1, 3);
        const owned = zlink.Message.from('owned');
        await right.send().message(prefix).message('text').message(bytes).message(owned).submit();
        strict_1.default.equal(owned.size(), 0);
        strict_1.default.equal(prefix.toString(), 'prefix');
        strict_1.default.equal(left.recv(received), true);
        strict_1.default.deepEqual(received.parts.map((part) => part.data()), [prefix, Buffer.from('text'), Buffer.from([2, 3]), Buffer.from('owned')]);
    }
    finally {
        received.close();
        right.close();
        left.close();
        context.close();
    }
});
