"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const completion_poller_1 = require("./completion_poller");
const zlink = require('@zlink-systems/zlink');
const payloads = [Buffer.alloc(21, 1), Buffer.alloc(0), Buffer.alloc(1024, 2)];
function send(socket) {
    socket.send().message(payloads[0]).message(payloads[1]).message(payloads[2]).submit_sync();
}
function assertAllocatorStorage(parts, allocated) {
    const views = parts.map((part) => part.data());
    strict_1.default.deepEqual(views, payloads);
    // Check actual allocator results, rather than platform-specific heap sizes.
    for (const view of views)
        strict_1.default.ok(allocated.includes(view));
    strict_1.default.equal(allocated.filter((buffer) => views.some((view) => view === buffer)).length, payloads.length);
}
(0, node_test_1.default)('receive adopts standard allocator storage and writes every payload byte', () => {
    const context = zlink.createContext();
    const left = zlink.createPairSocket(context);
    const right = zlink.createPairSocket(context);
    const received = new zlink.Received();
    const original = Buffer.allocUnsafe;
    const allocated = [];
    try {
        left.bind('inproc://receive-allocator');
        right.connect('inproc://receive-allocator');
        Buffer.allocUnsafe = ((size) => {
            const buffer = original(size).fill(0xa5);
            allocated.push(buffer);
            return buffer;
        });
        strict_1.default.equal(left.recv(received, zlink.RecvFlags.DontWait), false);
        strict_1.default.equal(allocated.length, 0, 'NO_DATA must not allocate payload storage');
        send(right);
        strict_1.default.equal(left.recv(received), true);
        strict_1.default.equal(allocated.length, payloads.length);
        assertAllocatorStorage(received.parts, allocated);
        received.parts[0].close();
        strict_1.default.deepEqual(received.parts[2].data(), payloads[2]);
    }
    finally {
        Buffer.allocUnsafe = original;
        received.close();
        right.close();
        left.close();
        context.close();
    }
});
(0, node_test_1.default)('receive backing allocations match the standard allocator baseline', (t) => {
    const records = 64;
    const baseline = new Set();
    for (let index = 0; index < records; ++index) {
        for (const payload of payloads) {
            const buffer = Buffer.allocUnsafe(payload.length);
            if (buffer.length > 0)
                baseline.add(buffer.buffer);
        }
    }
    const context = zlink.createContext();
    const left = zlink.createPairSocket(context);
    const right = zlink.createPairSocket(context);
    const received = new zlink.Received();
    const actual = new Set();
    try {
        left.bind('inproc://receive-allocation-count');
        right.connect('inproc://receive-allocation-count');
        for (let index = 0; index < records; ++index) {
            send(right);
            strict_1.default.equal(left.recv(received), true);
            strict_1.default.deepEqual(received.parts.map((part) => part.data()), payloads);
            for (const part of received.parts) {
                const buffer = part.data();
                if (buffer.length > 0)
                    actual.add(buffer.buffer);
            }
        }
        t.diagnostic(JSON.stringify({ records, standard: baseline.size, receive: actual.size }));
        // The two runs may begin at different offsets in Node's current slab.
        strict_1.default.ok(actual.size <= baseline.size + 1);
    }
    finally {
        received.close();
        right.close();
        left.close();
        context.close();
    }
});
(0, node_test_1.default)('request completions and routed refills adopt standard allocator storage', async () => {
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    const driver = new completion_poller_1.CompletionPollerDriver(context, dealer);
    const received = new zlink.Received();
    const original = Buffer.allocUnsafe;
    const allocated = [];
    try {
        router.bind('inproc://completion-allocator');
        dealer.connect('inproc://completion-allocator');
        Buffer.allocUnsafe = ((size) => {
            const buffer = original(size).fill(0xa5);
            allocated.push(buffer);
            return buffer;
        });
        for (let round = 0; round < 2; ++round) {
            const submission = dealer.request().timeout(1000)
                .message(payloads[0]).message(payloads[1]).message(payloads[2]).submit();
            strict_1.default.equal(router.recv(received), true);
            strict_1.default.deepEqual(received.parts.map((part) => part.data()), payloads);
            if (round > 0)
                assertAllocatorStorage(received.parts, allocated);
            received.reply().message(payloads[0]).message(payloads[1]).message(payloads[2]).submit();
            const parts = await driver.settle(submission.reply);
            try {
                assertAllocatorStorage(parts, allocated);
            }
            finally {
                for (const part of parts)
                    part.close();
            }
        }
    }
    finally {
        Buffer.allocUnsafe = original;
        driver.close();
        received.close();
        dealer.close();
        router.close();
        context.close();
    }
});
(0, node_test_1.default)('allocator reentry preserves both receive records', () => {
    const context = zlink.createContext();
    const left = zlink.createPairSocket(context);
    const right = zlink.createPairSocket(context);
    const otherLeft = zlink.createPairSocket(context);
    const otherRight = zlink.createPairSocket(context);
    const received = new zlink.Received();
    const otherReceived = new zlink.Received();
    const original = Buffer.allocUnsafe;
    let reentered = false;
    try {
        left.bind('inproc://allocator-reentry');
        right.connect('inproc://allocator-reentry');
        otherLeft.bind('inproc://allocator-nested');
        otherRight.connect('inproc://allocator-nested');
        send(right);
        otherRight.send().message('nested').message('record').submit_sync();
        Buffer.allocUnsafe = ((size) => {
            if (!reentered) {
                reentered = true;
                strict_1.default.equal(otherLeft.recv(otherReceived), true);
            }
            return original(size);
        });
        strict_1.default.equal(left.recv(received), true);
        strict_1.default.equal(reentered, true);
        strict_1.default.deepEqual(received.parts.map((part) => part.data()), payloads);
        strict_1.default.deepEqual(otherReceived.parts.map((part) => part.getString()), ['nested', 'record']);
    }
    finally {
        Buffer.allocUnsafe = original;
        otherReceived.close();
        received.close();
        otherRight.close();
        otherLeft.close();
        right.close();
        left.close();
        context.close();
    }
});
(0, node_test_1.default)('an undersized allocator result fails safely and releases the record', () => {
    const context = zlink.createContext();
    const left = zlink.createPairSocket(context);
    const right = zlink.createPairSocket(context);
    const received = new zlink.Received();
    const original = Buffer.allocUnsafe;
    try {
        left.bind('inproc://allocator-failure');
        right.connect('inproc://allocator-failure');
        send(right);
        Buffer.allocUnsafe = (() => Buffer.alloc(0));
        strict_1.default.throws(() => left.recv(received));
        Buffer.allocUnsafe = original;
        send(right);
        strict_1.default.equal(left.recv(received), true);
        strict_1.default.deepEqual(received.parts.map((part) => part.data()), payloads);
    }
    finally {
        Buffer.allocUnsafe = original;
        received.close();
        right.close();
        left.close();
        context.close();
    }
});
