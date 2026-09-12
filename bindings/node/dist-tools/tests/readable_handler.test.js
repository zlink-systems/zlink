"use strict";
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_child_process_1 = require("node:child_process");
const node_os_1 = require("node:os");
const node_path_1 = __importDefault(require("node:path"));
const promises_1 = require("node:timers/promises");
const zlink = require('@zlink-systems/zlink');
const { completionOwnerOf } = require('../../dist/zlink/runtime/messaging/completion_owner');
(0, node_test_1.default)('readable handler replaces its predecessor and drains a queued batch through no data', async () => {
    const ctx = zlink.createContext();
    const sender = zlink.createPairSocket(ctx);
    const receiver = zlink.createPairSocket(ctx);
    const received = new zlink.Received();
    sender.bind('inproc://readable-handler-batch');
    receiver.connect('inproc://readable-handler-batch');
    let replacedCalls = 0;
    let drainedToNoData = false;
    const values = [];
    try {
        receiver.setReadableHandler(() => { replacedCalls += 1; });
        const ready = new Promise((resolve, reject) => {
            receiver.setReadableHandler(function () {
                try {
                    strict_1.default.equal(arguments.length, 0);
                    while (receiver.recv(received, zlink.RecvFlags.DontWait)) {
                        values.push(received.parts[0].getString());
                        received.close();
                    }
                    drainedToNoData = true;
                    if (values.length === 4)
                        resolve();
                }
                catch (error) {
                    reject(error);
                }
            });
        });
        for (let index = 0; index < 4; index += 1)
            sender.send().message(String(index)).submit_sync();
        await ready;
        strict_1.default.deepEqual(values, ['0', '1', '2', '3']);
        strict_1.default.equal(drainedToNoData, true);
        strict_1.default.equal(replacedCalls, 0);
    }
    finally {
        received.close();
        receiver.close();
        sender.close();
        ctx.close();
    }
});
(0, node_test_1.default)('readable handler alone keeps the event loop alive and socket close releases it', () => {
    const packagePath = node_path_1.default.resolve(__dirname, '../../dist');
    const child = (0, node_child_process_1.spawnSync)(process.execPath, ['-e', `
    const assert = require('node:assert/strict');
    const z = require(${JSON.stringify(packagePath)});
    const ctx = z.createContext();
    const sender = z.createPairSocket(ctx);
    const receiver = z.createPairSocket(ctx);
    const received = new z.Received();
    sender.bind('inproc://readable-handler-lifetime');
    receiver.connect('inproc://readable-handler-lifetime');
    receiver.setReadableHandler(function () {
      assert.equal(arguments.length, 0);
      let delivered = false;
      while (receiver.recv(received, z.RecvFlags.DontWait)) {
        assert.equal(received.parts[0].getString(), 'wake');
        received.close();
        delivered = true;
      }
      if (delivered) {
        received.close(); receiver.close(); sender.close(); ctx.close();
        process.stdout.write('closed');
      }
    });
    setTimeout(() => sender.send().message('wake').submit_sync(), 25).unref();
  `], { encoding: 'utf8', timeout: 5000 });
    strict_1.default.equal(child.error, undefined, child.error?.message);
    strict_1.default.equal(child.status, 0, child.stderr);
    strict_1.default.equal(child.stdout, 'closed', child.stderr);
});
for (const paused of [false, true]) {
    (0, node_test_1.default)(`holding readable DATA for an application permit does not repeat idle notifications (paused=${paused})`, async () => {
        const ctx = zlink.createContext();
        const sender = zlink.createRouterSocket(ctx);
        const receiver = zlink.createDealerSocket(ctx);
        const received = new zlink.Received();
        const rid = zlink.RoutingId.from(Buffer.from('held-receiver'));
        receiver.setRoutingId(rid);
        sender.bind(`inproc://readable-held-${paused}`);
        receiver.connect(`inproc://readable-held-${paused}`);
        let notifications = 0;
        let armed = false;
        let ready;
        let arm;
        const initial = new Promise(resolve => { arm = resolve; });
        const readable = new Promise(resolve => { ready = resolve; });
        try {
            receiver.setReadableHandler(() => {
                if (!armed) {
                    while (receiver.recv(received, zlink.RecvFlags.DontWait))
                        received.close();
                    armed = true;
                    arm();
                    return;
                }
                notifications += 1;
                ready();
            });
            await initial;
            sender.send(rid).message('held').submit_sync();
            await readable;
            if (paused)
                receiver.setReceiveFlowState(zlink.ReceiveFlowState.Paused);
            await (0, promises_1.setTimeout)(10);
            const notificationsAfterProgress = notifications;
            await (0, promises_1.setTimeout)(20);
            strict_1.default.equal(notifications, notificationsAfterProgress, 'without new socket progress, an unread record must not keep waking the event loop');
            strict_1.default.equal(receiver.recv(received, zlink.RecvFlags.DontWait), true);
            strict_1.default.equal(received.parts[0].getString(), 'held');
            received.close();
            strict_1.default.equal(receiver.recv(received, zlink.RecvFlags.DontWait), false);
        }
        finally {
            received.close();
            receiver.close();
            sender.close();
            ctx.close();
        }
    });
}
(0, node_test_1.default)('watch acknowledgement preserves typed request termination during context shutdown', async () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    const request = new zlink.Received();
    const incoming = new zlink.Received();
    const receiveFailures = [];
    let initialized;
    const initial = new Promise(resolve => { initialized = resolve; });
    router.bind('inproc://readable-context-shutdown');
    dealer.connect('inproc://readable-context-shutdown');
    try {
        dealer.setReadableHandler(() => {
            try {
                while (dealer.recv(incoming, zlink.RecvFlags.DontWait))
                    incoming.close();
            }
            catch (error) {
                receiveFailures.push(error);
            }
            initialized();
        });
        await initial;
        const pending = dealer.request().message('pending').timeout(1000).submit().reply;
        const rejected = strict_1.default.rejects(pending, (error) => error instanceof zlink.RequestError && error.result === zlink.RequestResult.Terminated);
        strict_1.default.equal(router.recv(request), true);
        request.close();
        ctx.shutdown();
        await rejected;
        strict_1.default.ok(receiveFailures.some(error => error instanceof zlink.RecvError));
    }
    finally {
        request.close();
        incoming.close();
        dealer.close();
        router.close();
        ctx.close();
    }
});
(0, node_test_1.default)('one readable watch serves replacement handlers and completion ownership transfers', () => {
    const ctx = zlink.createContext();
    const socket = zlink.createPairSocket(ctx);
    const owner = completionOwnerOf(socket);
    let wake;
    let starts = 0;
    let stops = 0;
    let calls = 0;
    owner.native = {
        socketReadableWatchStart: (_handle, callback) => {
            starts += 1;
            wake = callback;
            return {};
        },
        socketReadableWatchStop: () => { stops += 1; },
    };
    try {
        socket.setReadableHandler(() => { calls += 100; });
        socket.setReadableHandler(() => { calls += 1; });
        const publicOwner = {};
        owner.transferToPublic(publicOwner);
        wake(0);
        owner.transferToRuntime(publicOwner);
        wake(0);
        strict_1.default.equal(starts, 1);
        strict_1.default.equal(stops, 0);
        strict_1.default.equal(calls, 2);
        strict_1.default.throws(() => socket.setReadableHandler(null), (error) => error instanceof zlink.HandlerError && error.result === zlink.HandlerResult.InvalidArgument);
        socket.close();
        wake(0);
        strict_1.default.equal(calls, 2);
        strict_1.default.equal(stops, 1);
        strict_1.default.throws(() => socket.setReadableHandler(() => { }), (error) => error instanceof zlink.HandlerError && error.result === zlink.HandlerResult.InvalidHandle);
    }
    finally {
        socket.close();
        ctx.close();
    }
});
(0, node_test_1.default)('readable notifications coexist with runtime and public request completion drains', async () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    const request = new zlink.Received();
    const data = new zlink.Received();
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    router.bind('inproc://readable-handler-completions');
    dealer.connect('inproc://readable-handler-completions');
    let served;
    let delivered;
    let failServing;
    let failDelivery;
    const values = [];
    try {
        router.setReadableHandler(() => {
            try {
                while (router.recv(request, zlink.RecvFlags.DontWait)) {
                    const value = request.parts[0].getString();
                    const peer = request.routingId;
                    request.reply().message(`reply:${value}`).submit();
                    request.close();
                    router.send(peer).message(`data:${value}`).submit_sync();
                    served();
                }
            }
            catch (error) {
                failServing(error);
            }
        });
        dealer.setReadableHandler(() => {
            try {
                while (dealer.recv(data, zlink.RecvFlags.DontWait)) {
                    values.push(data.parts[0].getString());
                    data.close();
                    delivered();
                }
            }
            catch (error) {
                failDelivery(error);
            }
        });
        for (const mode of ['public', 'runtime']) {
            const servedRequest = new Promise((resolve, reject) => { served = resolve; failServing = reject; });
            const receivedData = new Promise((resolve, reject) => { delivered = resolve; failDelivery = reject; });
            const pending = dealer.request().message(mode).timeout(1000).submit().reply;
            if (mode === 'public')
                poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 7);
            await servedRequest;
            if (mode === 'public') {
                strict_1.default.equal(poller.wait(events, 1000), 1);
                strict_1.default.equal(events.hasEvent(0, zlink.PollEventFlag.PollCompletion), true);
            }
            const parts = await pending;
            try {
                strict_1.default.equal(parts[0].getString(), `reply:${mode}`);
            }
            finally {
                parts.forEach((part) => part.close());
            }
            await receivedData;
            if (mode === 'public')
                strict_1.default.equal(poller.remove(dealer), true);
        }
        strict_1.default.deepEqual(values, ['data:public', 'data:runtime']);
    }
    finally {
        events.close();
        poller.close();
        request.close();
        data.close();
        dealer.close();
        router.close();
        ctx.close();
    }
});
(0, node_test_1.default)('watch failure rejects all pending operations and reaches the handler only through its next receive', async () => {
    const ctx = zlink.createContext();
    const socket = zlink.createPairSocket(ctx);
    const received = new zlink.Received();
    const owner = completionOwnerOf(socket);
    let wake;
    let calls = 0;
    let stops = 0;
    let requestCount = 0;
    owner.native = {
        socketReadableWatchStart: (_handle, callback) => { wake = callback; return {}; },
        socketReadableWatchStop: () => { stops += 1; },
        socketSubmitSend: () => ({ result: zlink.SubmitResult.Backpressured, nativeErrno: node_os_1.constants.errno.EAGAIN, completionId: 11n }),
        socketSubmitRequest: () => ++requestCount === 1
            ? { result: zlink.SubmitResult.Ok, nativeErrno: 0, completionId: 12n }
            : { result: zlink.SubmitResult.Backpressured, nativeErrno: node_os_1.constants.errno.EAGAIN, completionId: 13n },
    };
    try {
        socket.setReadableHandler(function () {
            calls += 1;
            strict_1.default.equal(arguments.length, 0);
            strict_1.default.throws(() => socket.recv(received, zlink.RecvFlags.DontWait), (error) => error instanceof zlink.RecvError && error.result === zlink.RecvResult.InternalError
                && error.nativeErrno === -9 && /readable watch failed/.test(error.message));
            strict_1.default.equal(socket.recv(received, zlink.RecvFlags.DontWait), false);
        });
        const send = owner.submitSend(Buffer.from('send'), null);
        const request = owner.submitRequest(Buffer.from('request'), null, 1000);
        const queuedRequest = owner.submitRequest(Buffer.from('queued'), null, 1000);
        const submitFailed = (error) => error instanceof zlink.SubmitError
            && error.result === zlink.SubmitResult.InternalError;
        const failed = [
            strict_1.default.rejects(send.admitted, submitFailed),
            strict_1.default.rejects(request.reply, (error) => error instanceof zlink.RequestError
                && error.result === zlink.RequestResult.InternalError),
            strict_1.default.rejects(queuedRequest.admitted, submitFailed),
            strict_1.default.rejects(queuedRequest.reply, submitFailed),
        ];
        strict_1.default.equal(socket.recv(received, zlink.RecvFlags.DontWait), false);
        wake(-9);
        await Promise.all(failed);
        strict_1.default.equal(calls, 1);
        strict_1.default.equal(stops, 1);
        strict_1.default.equal(owner.hasManagedWritableWait(), false);
        socket.close();
        wake(0);
        strict_1.default.equal(calls, 1);
    }
    finally {
        received.close();
        socket.close();
        ctx.close();
    }
});
for (const [factory, receive, resultType] of [
    ['createPairSocket', 'recv', 'Received'],
    ['createDealerSocket', 'recv', 'Received'],
    ['createRouterSocket', 'recv', 'Received'],
    ['createStreamSocket', 'recv', 'Received'],
    ['createStreamSocket', 'recvPacket', 'StreamPacket'],
    ['createSubSocket', 'subscribe', 'TopicMessage'],
    ['createXSubSocket', 'subscribe', 'TopicMessage'],
    ['createXPubSocket', 'receiveSubscriptionEvent', 'SubscriptionEvent'],
]) {
    (0, node_test_1.default)(`${factory}.${receive} consumes its watch failure once without stale errno remapping`, () => {
        const ctx = zlink.createContext();
        const socket = zlink[factory](ctx);
        const result = new zlink[resultType]();
        const owner = completionOwnerOf(socket);
        let wake;
        owner.native = {
            socketReadableWatchStart: (_handle, callback) => { wake = callback; return {}; },
            socketReadableWatchStop: () => { },
        };
        try {
            if (factory === 'createStreamSocket') {
                socket.options.recvMode = receive === 'recvPacket'
                    ? zlink.StreamRecvMode.Packet : zlink.StreamRecvMode.Raw;
            }
            socket.setReadableHandler(() => { });
            strict_1.default.equal(socket[receive](result, zlink.RecvFlags.DontWait), false);
            wake(-9);
            strict_1.default.throws(() => socket[receive](result, zlink.RecvFlags.DontWait), (error) => error instanceof zlink.RecvError && error.result === zlink.RecvResult.InternalError
                && error.nativeErrno === -9);
            strict_1.default.equal(socket[receive](result, zlink.RecvFlags.DontWait), false);
        }
        finally {
            if (typeof result.close === 'function')
                result.close();
            socket.close();
            ctx.close();
        }
    });
}
