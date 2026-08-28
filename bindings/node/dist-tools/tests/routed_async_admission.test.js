// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const { Worker } = require('node:worker_threads');
const zlink = require('@zlink-systems/zlink');
let endpointSequence = 0;
function endpoint(label) {
    endpointSequence += 1;
    return `inproc://node-send-completion-${label}-${process.pid}-${endpointSequence}`;
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
test('managed PAIR send resolves from Core completion and consumes at submit', async () => {
    const context = zlink.createContext();
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    sender.bind(endpoint('inline'));
    receiver.connect(sender.options.lastEndpoint);
    const payload = zlink.Message.from('completion');
    try {
        const result = sender.send().message(payload).submit();
        assert.equal(typeof result.then, 'function');
        await result;
        assert.equal(payload.size(), 0);
        const received = new zlink.Received();
        assert.equal(receiver.recv(received), true);
        assert.equal(received.singlePartOrThrow().getString(), 'completion');
        received.close();
    }
    finally {
        closeAll(context, sender, receiver);
        payload.close();
    }
});
test('sync PAIR submit_sync(NONE) admits a send and returns void', () => {
    const context = zlink.createContext();
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    sender.bind(endpoint('sync-blocking'));
    receiver.connect(sender.options.lastEndpoint);
    try {
        const result = sender.send().message('sync-completion').submit_sync(zlink.SendFlags.None);
        assert.equal(result, undefined);
        const received = new zlink.Received();
        assert.equal(receiver.recv(received), true);
        assert.equal(received.singlePartOrThrow().getString(), 'sync-completion');
        received.close();
    }
    finally {
        closeAll(context, sender, receiver);
    }
});
test('sync PAIR submit_sync(DONTWAIT) reports immediate backpressure at HWM', async () => {
    const context = zlink.createContext();
    context.options.autoHwmEnabled = false;
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    sender.options.sendHwm = 4096n;
    receiver.options.recvHwm = 4096n;
    sender.bind(endpoint('sync-dontwait'));
    receiver.connect(sender.options.lastEndpoint);
    const first = zlink.Message.from(Buffer.alloc(4_096, 0x65));
    const rejected = zlink.Message.from(Buffer.alloc(4_096, 0x66));
    try {
        await sender.send().message(first).submit();
        const startedAt = Date.now();
        assert.throws(() => sender.send().message(rejected).submit_sync(zlink.SendFlags.DontWait), (error) => error instanceof zlink.SubmitError
            && error.result === zlink.SubmitResult.Backpressured);
        assert.ok(Date.now() - startedAt < 250, 'DONTWAIT submit must return immediately');
        assert.equal(rejected.size(), 4_096);
    }
    finally {
        closeAll(context, sender, receiver);
        first.close();
        rejected.close();
    }
});
test('Core timeout maps to per-operation SubmitError with the Core errno', async () => {
    const context = zlink.createContext();
    context.options.autoHwmEnabled = false;
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    sender.options.sendHwm = 4096n;
    receiver.options.recvHwm = 4096n;
    sender.bind(endpoint('timeout'));
    receiver.connect(sender.options.lastEndpoint);
    // Multipart routed records remain one-part here until the parallel Core
    // multipart ROUTER/DEALER defect fix lands.
    const first = zlink.Message.from(Buffer.alloc(4_096, 0x61));
    const pending = zlink.Message.from(Buffer.alloc(4_096, 0x62));
    try {
        await sender.send().message(first).submit();
        await assert.rejects(within(sender.send().message(pending).timeout(20).submit()), (error) => error instanceof zlink.SubmitError
            && error.result === zlink.SubmitResult.Backpressured
            && error.nativeErrno !== 0);
        // Ownership transfers when zlink_send_async returns OK, even when the
        // eventual Core completion is TIMED_OUT.
        assert.equal(pending.size(), 0);
    }
    finally {
        closeAll(context, sender, receiver);
        first.close();
        pending.close();
    }
});
test('Core terminal completion rejects a pending send without a binding cancel API', async () => {
    const context = zlink.createContext();
    context.options.autoHwmEnabled = false;
    const sender = zlink.createPairSocket(context);
    const receiver = zlink.createPairSocket(context);
    sender.options.sendHwm = 4096n;
    receiver.options.recvHwm = 4096n;
    sender.bind(endpoint('terminal'));
    receiver.connect(sender.options.lastEndpoint);
    const first = zlink.Message.from(Buffer.alloc(4_096, 0x63));
    const pending = zlink.Message.from(Buffer.alloc(4_096, 0x64));
    try {
        await sender.send().message(first).submit();
        const result = sender.send().message(pending).timeout(-1).submit();
        await nextTurn();
        sender.close();
        await assert.rejects(within(result), (error) => error instanceof zlink.SubmitError
            && error.result === zlink.SubmitResult.Terminated
            && error.nativeErrno !== 0);
        assert.equal(pending.size(), 0);
    }
    finally {
        closeAll(context, sender, receiver);
        first.close();
        pending.close();
    }
});
test('request Promise is settled only by the Core reply callback', async () => {
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    dealer.setRoutingId(zlink.RoutingId.from('request-client'));
    router.bind(endpoint('request'));
    dealer.connect(router.options.lastEndpoint);
    try {
        const replyPromise = dealer.request().message('request').timeout(1_000).submit();
        let request = null;
        for (let attempt = 0; attempt < 100 && !request; attempt += 1) {
            const candidate = new zlink.Received();
            if (router.recv(candidate, zlink.RecvFlags.DontWait))
                request = candidate;
            else
                candidate.close();
            if (!request)
                await nextTurn();
        }
        assert.ok(request);
        request.reply().message('reply').submit();
        const parts = await within(replyPromise);
        assert.equal(parts.length, 1);
        assert.equal(parts[0].getString(), 'reply');
        parts[0].close();
        request.close();
    }
    finally {
        closeAll(context, dealer, router);
    }
});
test('request submit_sync(DONTWAIT, callback) returns after admission and delivers reply', async () => {
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    router.bind(endpoint('request-callback'));
    dealer.connect(router.options.lastEndpoint);
    try {
        const completion = new Promise((resolve, reject) => {
            const result = dealer.request().message('callback-request').timeout(1_000)
                .submit_sync(zlink.SendFlags.DontWait, (error, reply) => {
                if (error)
                    reject(error);
                else
                    resolve(reply ?? []);
            });
            assert.equal(result, undefined);
        });
        let request = null;
        for (let i = 0; i < 100 && !request; i += 1) {
            const candidate = new zlink.Received();
            if (router.recv(candidate, zlink.RecvFlags.DontWait))
                request = candidate;
            else
                candidate.close();
            if (!request)
                await nextTurn();
        }
        assert.ok(request);
        request.reply().message('callback-reply').submit();
        const reply = await within(completion);
        assert.equal(reply[0].getString(), 'callback-reply');
        reply[0].close();
        request.close();
    }
    finally {
        closeAll(context, dealer, router);
    }
});
test('PollCompletion delivers submit_sync callback before wait returns', async () => {
    const controlBuffer = new SharedArrayBuffer(4);
    const control = new Int32Array(controlBuffer);
    const worker = new Worker(`
    const { parentPort, workerData } = require('node:worker_threads');
    const z = require(workerData.modulePath);
    const control = new Int32Array(workerData.controlBuffer);
    const ctx = z.createContext(); const router = z.createRouterSocket(ctx);
    router.bind('tcp://127.0.0.1:*'); parentPort.postMessage(router.options.lastEndpoint);
    const received = new z.Received(); router.recv(received, z.RecvFlags.None);
    while (Atomics.load(control, 0) === 0) Atomics.wait(control, 0, 0);
    received.reply().message('sync-progress-reply').submit(); received.close();
    router.close(); ctx.close();
  `, {
        eval: true,
        workerData: {
            modulePath: require.resolve('@zlink-systems/zlink'),
            controlBuffer,
        }
    });
    const remoteEndpoint = await new Promise((resolve, reject) => {
        worker.once('message', resolve);
        worker.once('error', reject);
    });
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    dealer.connect(remoteEndpoint);
    try {
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50);
        let callbackError = null;
        let callbackReply = null;
        let waitReturned = false;
        let callbackBeforeWaitReturned = false;
        dealer.request().message('sync-progress-request').timeout(2_000)
            .submit_sync(zlink.SendFlags.DontWait, (error, reply) => {
            callbackError = error;
            callbackReply = reply;
            callbackBeforeWaitReturned = !waitReturned;
        });
        poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 0);
        Atomics.store(control, 0, 1);
        Atomics.notify(control, 0);
        poller.wait(events, 2_000);
        waitReturned = true;
        assert.equal(callbackError, null);
        assert.equal(callbackBeforeWaitReturned, true);
        assert.ok(callbackReply);
        assert.equal(callbackReply[0].getString(), 'sync-progress-reply');
        callbackReply[0].close();
    }
    finally {
        events.close();
        poller.close();
        closeAll(context, dealer);
        await worker.terminate();
    }
});
test('routed PollCompletion delivers submit_sync callback before wait returns', async () => {
    const controlBuffer = new SharedArrayBuffer(4);
    const control = new Int32Array(controlBuffer);
    const worker = new Worker(`
    const { parentPort, workerData } = require('node:worker_threads');
    const z = require(workerData.modulePath);
    const control = new Int32Array(workerData.controlBuffer);
    const ctx = z.createContext(); const router = z.createRouterSocket(ctx);
    router.setRoutingId(z.RoutingId.from(Buffer.from('SERVER')));
    router.options.mandatory = true;
    router.bind('tcp://127.0.0.1:*'); parentPort.postMessage(router.options.lastEndpoint);
    const received = new z.Received(); router.recv(received, z.RecvFlags.None);
    while (Atomics.load(control, 0) === 0) Atomics.wait(control, 0, 0);
    received.reply().message('routed-sync-progress-reply').submit(); received.close();
    router.close(); ctx.close();
  `, {
        eval: true,
        workerData: {
            modulePath: require.resolve('@zlink-systems/zlink'),
            controlBuffer,
        }
    });
    const remoteEndpoint = await new Promise((resolve, reject) => {
        worker.once('message', resolve);
        worker.once('error', reject);
    });
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    const serverRoutingId = zlink.RoutingId.from(Buffer.from('SERVER'));
    router.setRoutingId(zlink.RoutingId.from(Buffer.from('CLIENT')));
    router.options.setConnectRoutingId(serverRoutingId);
    router.options.mandatory = true;
    router.connect(remoteEndpoint);
    try {
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50);
        let callbackError = null;
        let callbackReply = null;
        let waitReturned = false;
        let callbackBeforeWaitReturned = false;
        router.request(serverRoutingId).message('routed-sync-progress-request').timeout(2_000)
            .submit_sync(zlink.SendFlags.DontWait, (error, reply) => {
            callbackError = error;
            callbackReply = reply;
            callbackBeforeWaitReturned = !waitReturned;
        });
        poller.add(router, [zlink.PollEventFlag.PollCompletion], 0);
        Atomics.store(control, 0, 1);
        Atomics.notify(control, 0);
        poller.wait(events, 2_000);
        waitReturned = true;
        assert.equal(callbackError, null);
        assert.equal(callbackBeforeWaitReturned, true);
        assert.ok(callbackReply);
        assert.equal(callbackReply[0].getString(), 'routed-sync-progress-reply');
        callbackReply[0].close();
    }
    finally {
        events.close();
        poller.close();
        closeAll(context, router);
        await worker.terminate();
    }
});
test('PollCompletion synchronously delivers submit_sync request timeout', () => {
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    const router = zlink.createRouterSocket(context);
    const poller = zlink.createPoller();
    const events = zlink.createPollEvents(1);
    router.bind(endpoint('sync-progress-timeout'));
    dealer.connect(router.options.lastEndpoint);
    try {
        let callbackError = null;
        let callbackReply = null;
        let waitReturned = false;
        let callbackBeforeWaitReturned = false;
        dealer.request().message('never-replied').timeout(25)
            .submit_sync(zlink.SendFlags.DontWait, (error, reply) => {
            callbackError = error;
            callbackReply = reply;
            callbackBeforeWaitReturned = !waitReturned;
        });
        poller.add(dealer, [zlink.PollEventFlag.PollCompletion], 0);
        poller.wait(events, 1_000);
        waitReturned = true;
        assert.ok(callbackError instanceof zlink.RequestError);
        assert.equal(callbackError.result, zlink.RequestResult.TimedOut);
        assert.equal(callbackReply, null);
        assert.equal(callbackBeforeWaitReturned, true);
    }
    finally {
        events.close();
        poller.close();
        closeAll(context, dealer, router);
    }
});
test('request submit_sync(DONTWAIT, callback) throws SubmitError on admission backpressure', async () => {
    const context = zlink.createContext();
    context.options.autoHwmEnabled = false;
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    dealer.options.sendHwm = 4096n;
    router.options.recvHwm = 4096n;
    router.bind(endpoint('request-callback-backpressure'));
    dealer.connect(router.options.lastEndpoint);
    const callback = (_error, reply) => reply?.forEach((part) => part.close());
    let backpressured = false;
    try {
        for (let i = 0; i < 100 && !backpressured; i += 1) {
            try {
                dealer.request().message(Buffer.alloc(4_096, i)).timeout(100)
                    .submit_sync(zlink.SendFlags.DontWait, callback);
            }
            catch (error) {
                assert.ok(error instanceof zlink.SubmitError);
                assert.equal(error.result, zlink.SubmitResult.Backpressured);
                backpressured = true;
            }
        }
        assert.equal(backpressured, true);
    }
    finally {
        closeAll(context, dealer, router);
    }
});
test('request submit_sync(NONE) blocks natively and returns reply parts', async () => {
    const worker = new Worker(`
    const { parentPort, workerData } = require('node:worker_threads');
    const z = require(workerData.modulePath);
    const ctx = z.createContext(); const router = z.createRouterSocket(ctx);
    router.bind('tcp://127.0.0.1:*'); parentPort.postMessage(router.options.lastEndpoint);
    const poll = () => {
      const received = new z.Received();
      if (router.recv(received, z.RecvFlags.DontWait)) {
        received.reply().message('sync-reply').submit(); received.close();
        router.close(); ctx.close(); parentPort.postMessage('done'); return;
      }
      received.close(); setImmediate(poll);
    }; poll();
  `, { eval: true, workerData: { modulePath: require.resolve('@zlink-systems/zlink') } });
    const remoteEndpoint = await new Promise((resolve, reject) => {
        worker.once('message', resolve);
        worker.once('error', reject);
    });
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    dealer.connect(remoteEndpoint);
    try {
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50);
        const reply = dealer.request().message('sync-request').timeout(2_000)
            .submit_sync(zlink.SendFlags.None);
        assert.equal(reply[0].getString(), 'sync-reply');
        reply[0].close();
    }
    finally {
        closeAll(context, dealer);
        await worker.terminate();
    }
});
