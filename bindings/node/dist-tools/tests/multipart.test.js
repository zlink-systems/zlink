'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
process.env.ZLINK_NODE_TEST_HOOKS = '1';
const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const zlink = require('@zlink-systems/zlink');
const nativeTestHooks = require(path.resolve(__dirname, '../../build/Release/zlink.node'));
const { getNativeHandle } = require(path.resolve(__dirname, '../../dist/zlink/runtime/handles/native_handle.js'));
test('pair sockets send and receive multipart through canonical api', () => {
    const ctx = zlink.createContext();
    const left = zlink.createPairSocket(ctx);
    const right = zlink.createPairSocket(ctx);
    left.bind('inproc://multipart-contract');
    right.connect('inproc://multipart-contract');
    right.send().message('a').message(Buffer.from('b')).submit();
    const received = new zlink.Received();
    left.recv(received);
    assert.deepEqual(received.parts.map((part) => part.data().toString()), ['a', 'b']);
    right.close();
    left.close();
    ctx.close();
});
test('concurrent routed multipart is safely admitted and sent after the held record without mixing', async () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealer = zlink.createDealerSocket(ctx);
    const inbound = new zlink.Received();
    const heldInbound = new zlink.Received();
    const first = zlink.Message.from('queued-first');
    const second = zlink.Message.from('queued-second');
    let held;
    try {
        router.bind('inproc://node-concurrent-multipart-contract');
        dealer.connect('inproc://node-concurrent-multipart-contract');
        await dealer.send().message('route-probe').submit();
        assert.equal(router.recv(inbound), true);
        assert.ok(inbound.routingId);
        held = nativeTestHooks.testBeginHeldRoutedMultipart(getNativeHandle(router), inbound.routingId.toBytes());
        assert.equal(held.openResult, zlink.SubmitResult.Ok, `open errno=${held.openErrno}`);
        // Core admits this record as pending while the native thread owns an open
        // multipart record; submit must return normally with its completion Promise.
        const pendingSend = inbound.send()
            .message(first)
            .message(second)
            .submit();
        assert.ok(pendingSend instanceof Promise);
        const completed = nativeTestHooks.testEndHeldRoutedMultipart(held.state);
        held = undefined;
        assert.equal(completed.finalResult, zlink.SubmitResult.Ok, `final errno=${completed.finalErrno}`);
        await pendingSend;
        assert.equal(dealer.recv(heldInbound), true);
        assert.deepEqual(heldInbound.parts.map((part) => part.toString()), ['held-first', 'held-final']);
        assert.equal(dealer.recv(heldInbound), true);
        assert.deepEqual(heldInbound.parts.map((part) => part.toString()), ['queued-first', 'queued-second']);
    }
    finally {
        if (held)
            nativeTestHooks.testEndHeldRoutedMultipart(held.state);
        first.close();
        second.close();
        heldInbound.close();
        inbound.close();
        dealer.close();
        router.close();
        ctx.close();
    }
});
test('native thread stress mixes single-part, multipart, and close races', () => {
    const counts = nativeTestHooks.testRunSendCloseStress(4, 10_000);
    assert.equal(counts.attempts, 40000n);
    assert.equal(counts.single_attempts, 20000n);
    assert.equal(counts.multipart_attempts, 20000n);
    assert.equal(counts.submitted
        + counts.rejected_einval
        + counts.shutdown
        + counts.backpressured
        + counts.other_submit, counts.attempts);
    assert.ok(counts.submitted > 0n);
    assert.ok(counts.rejected_einval > 0n);
    assert.ok(counts.received_records > 0n);
    assert.equal(counts.bad_records, 0n);
    assert.equal(counts.close_ok, 1n);
    assert.equal(counts.close_shutdown, 0n);
    assert.equal(counts.close_other, 0n);
});
