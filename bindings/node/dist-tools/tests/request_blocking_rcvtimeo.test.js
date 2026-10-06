"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
const node_worker_threads_1 = require("node:worker_threads");
const node_events_1 = require("node:events");
const test = require("node:test");
const assert = require("node:assert/strict");
const zlink = require("@zlink-systems/zlink");
const receiveTimeoutMs = 1;
const replyDelayMs = 50;
const requestTimeoutMs = 500;
const providerReceiveTimeoutMs = 2_000;
// The provider runs on an independent thread while submit_sync blocks this thread.
test("blocking request waits beyond RCVTIMEO for the Core reply terminal", async () => {
    const provider = new node_worker_threads_1.Worker(`
    const { parentPort, workerData } = require('node:worker_threads');
    const zlink = require(workerData.bindingPath);
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    router.options.recvTimeout = workerData.receiveTimeoutMs;
    router.options.linger = 0;
    try {
      router.bind('tcp://127.0.0.1:*');
      parentPort.postMessage(router.options.lastEndpoint);
      const ready = new zlink.Received();
      try {
        if (!router.recv(ready) || ready.singlePartOrThrow().getString() !== 'ready')
          throw new Error('provider did not receive the ready handshake');
      } finally { ready.close(); }
      parentPort.postMessage('route-ready');
      const received = new zlink.Received();
      try {
        if (!router.recv(received)) throw new Error('provider did not receive the request');
        parentPort.postMessage('request-received');
        Atomics.wait(new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT)), 0, 0, workerData.replyDelayMs);
        received.reply().message('pong').submit();
      } finally { received.close(); }
    } finally { router.close(); context.close(); }
  `, {
        eval: true,
        workerData: {
            bindingPath: require.resolve("@zlink-systems/zlink"),
            receiveTimeoutMs: providerReceiveTimeoutMs,
            replyDelayMs,
        },
    });
    const [endpoint] = await (0, node_events_1.once)(provider, "message");
    const context = zlink.createContext();
    const dealer = zlink.createDealerSocket(context);
    dealer.options.recvTimeout = receiveTimeoutMs;
    dealer.options.linger = 0;
    dealer.connect(endpoint);
    try {
        dealer.send().message("ready").submit_sync();
        assert.equal((await (0, node_events_1.once)(provider, "message"))[0], "route-ready");
        let reply;
        let failure;
        try {
            reply = dealer
                .request()
                .message("ping")
                .timeout(requestTimeoutMs)
                .submit_sync();
        }
        catch (error) {
            failure = error;
        }
        assert.equal((await (0, node_events_1.once)(provider, "message"))[0], "request-received");
        if (failure !== undefined) {
            console.error(`Core completion receive failed after provider acceptance: result=${failure.result}, message=${failure.message}`);
            throw failure;
        }
        try {
            assert.equal(reply[0].getString(), "pong");
        }
        finally {
            reply.forEach((part) => part.close());
        }
    }
    finally {
        await provider.terminate();
        dealer.close();
        context.close();
    }
});
test("blocking request retains the Core deadline terminal across RCVTIMEO intervals", () => {
    const context = zlink.createContext();
    const router = zlink.createRouterSocket(context);
    const dealer = zlink.createDealerSocket(context);
    router.options.linger = dealer.options.linger = 0;
    router.bind("inproc://blocking-request-rcvtimeo-deadline");
    dealer.connect("inproc://blocking-request-rcvtimeo-deadline");
    dealer.options.recvTimeout = receiveTimeoutMs;
    try {
        dealer.send().message("ready").submit_sync();
        const ready = new zlink.Received();
        try {
            assert.equal(router.recv(ready), true);
        }
        finally {
            ready.close();
        }
        assert.throws(() => dealer
            .request()
            .message("no-reply")
            .timeout(replyDelayMs)
            .submit_sync(), (error) => error instanceof zlink.RequestError &&
            error.result === zlink.RequestResult.TimedOut);
    }
    finally {
        dealer.close();
        router.close();
        context.close();
    }
});
