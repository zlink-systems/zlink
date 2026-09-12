"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const zlink = require('@zlink-systems/zlink');
for (const routed of [false, true]) {
    for (const asynchronous of [false, true]) {
        (0, node_test_1.default)(`readable handler survives same-socket send (routed=${routed}, async=${asynchronous})`, async () => {
            const ctx = zlink.createContext();
            const sender = routed ? zlink.createRouterSocket(ctx) : zlink.createPairSocket(ctx);
            const receiver = routed ? zlink.createRouterSocket(ctx) : zlink.createPairSocket(ctx);
            const senderRid = zlink.RoutingId.from(Buffer.from('sender'));
            const receiverRid = zlink.RoutingId.from(Buffer.from('receiver'));
            const received = new zlink.Received();
            const values = [];
            let notifications = 0;
            let delivered;
            let failed;
            function nextReceipt() {
                return new Promise((resolve, reject) => {
                    const deadline = setTimeout(() => {
                        try {
                            // Inspect only after the notification deadline. This read cannot
                            // make a lost notification pass, and distinguishes it from transit delay.
                            const queued = receiver.recv(received, zlink.RecvFlags.DontWait);
                            const payload = queued ? received.parts[0].getString() : '<no data>';
                            received.close();
                            strict_1.default.equal(queued, false, `readiness lost: recv(DontWait) still returned ${payload}`);
                            strict_1.default.fail('readable handler deadline: no message remained queued (delivery delayed)');
                        }
                        catch (error) {
                            reject(error);
                        }
                    }, 1000);
                    delivered = () => { clearTimeout(deadline); resolve(); };
                    failed = error => { clearTimeout(deadline); reject(error); };
                });
            }
            async function send(socket, target, value) {
                const operation = (routed ? socket.send(target) : socket.send()).message(value);
                if (asynchronous)
                    await operation.submit().admitted;
                else
                    operation.submit_sync();
            }
            try {
                const endpoint = `inproc://readable-send-${routed}-${asynchronous}`;
                if (routed) {
                    sender.setRoutingId(senderRid);
                    receiver.setRoutingId(receiverRid);
                    sender.options.mandatory = true;
                    receiver.options.mandatory = true;
                    receiver.options.setConnectRoutingId(senderRid);
                }
                sender.bind(endpoint);
                receiver.connect(endpoint);
                let receipt = nextReceipt();
                receiver.setReadableHandler(function () {
                    try {
                        strict_1.default.equal(arguments.length, 0);
                        notifications += 1;
                        let read = false;
                        while (receiver.recv(received, zlink.RecvFlags.DontWait)) {
                            values.push(received.parts[0].getString());
                            received.close();
                            read = true;
                        }
                        if (read)
                            delivered();
                    }
                    catch (error) {
                        failed(error);
                    }
                });
                await send(sender, receiverRid, 'first');
                await receipt;
                strict_1.default.deepEqual(values, ['first']);
                const firstNotifications = notifications;
                receipt = nextReceipt();
                // Both sends run before libuv can observe the receiver's next mailbox
                // notification, matching the public Issue #77 reproduction.
                await send(sender, receiverRid, 'next');
                await send(receiver, senderRid, 'unrelated-outbound');
                await receipt;
                strict_1.default.deepEqual(values, ['first', 'next']);
                strict_1.default.ok(notifications > firstNotifications);
            }
            finally {
                received.close();
                receiver.close();
                sender.close();
                ctx.close();
            }
        });
    }
}
(0, node_test_1.default)('continuous readable replies yield to the event loop', async () => {
    const ctx = zlink.createContext();
    const left = zlink.createPairSocket(ctx);
    const right = zlink.createPairSocket(ctx);
    const received = new zlink.Received();
    let turns = 0;
    let loopAdvanced = false;
    let deadline;
    const immediate = setImmediate(() => { loopAdvanced = true; });
    try {
        left.bind('inproc://readable-send-fairness');
        right.connect('inproc://readable-send-fairness');
        const complete = new Promise((resolve, reject) => {
            deadline = setTimeout(() => reject(new Error('readable reply progress stopped')), 1000);
            for (const socket of [left, right]) {
                socket.setReadableHandler(() => {
                    try {
                        while (socket.recv(received, zlink.RecvFlags.DontWait)) {
                            received.close();
                            turns += 1;
                            if (turns === 128) {
                                strict_1.default.equal(loopAdvanced, true, 'readable work starved setImmediate');
                                resolve();
                            }
                            else {
                                socket.send().message('reply').submit_sync();
                            }
                        }
                    }
                    catch (error) {
                        reject(error);
                    }
                });
            }
        });
        left.send().message('start').submit_sync();
        await complete;
    }
    finally {
        clearTimeout(deadline);
        clearImmediate(immediate);
        received.close();
        right.close();
        left.close();
        ctx.close();
    }
});
