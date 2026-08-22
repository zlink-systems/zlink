'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
test('HWM and Core HWM budget options preserve uint64 byte values', () => {
    const ctx = zlink.createContext();
    ctx.options.autoHwmEnabled = false;
    const socket = zlink.createPairSocket(ctx);
    const maxBudget = (1n << 64n) - 1n;
    const maxHwm = (1n << 64n) - 1n;
    assert.equal(socket.options.sendHwm, 4096000n);
    assert.equal(socket.options.recvHwm, 4096000n);
    ctx.options.coreHwmMemoryLimitBytes = maxBudget;
    ctx.options.coreHwmBudgetBytes = maxBudget;
    assert.equal(ctx.options.coreHwmMemoryLimitBytes, maxBudget);
    assert.equal(ctx.options.coreHwmBudgetBytes, maxBudget);
    ctx.recalculateAutoHwm();
    const before = ctx.getCoreHwmBudgetSnapshot();
    assert.equal(before.abiVersion, 1);
    assert.equal(before.configuredMemoryLimitBytes, maxBudget);
    assert.equal(before.configuredCoreBudgetBytes, maxBudget);
    assert.equal(before.reservedUInt64.length, 8);
    ctx.resetCoreHwmBudgetMetrics();
    assert.ok(ctx.getCoreHwmBudgetSnapshot().measurementEpoch > before.measurementEpoch);
    socket.options.sendHwm = maxHwm;
    socket.options.recvHwm = 0n;
    assert.equal(socket.options.sendHwm, maxHwm);
    assert.equal(socket.options.recvHwm, 0n);
    assert.throws(() => { socket.options.sendHwm = 4096; }, TypeError);
    assert.throws(() => { ctx.options.coreHwmBudgetBytes = 4096; }, TypeError);
    assert.throws(() => { socket.options.recvHwm = -1n; }, RangeError);
    socket.close();
    ctx.close();
});
test('monitor ABI v4 exposes byte telemetry as bigint', () => {
    const ctx = zlink.createContext();
    const socket = zlink.createPairSocket(ctx);
    const monitorHwmBytes = 12345n;
    const monitor = socket.monitorOpen(undefined, monitorHwmBytes);
    const status = monitor.status();
    assert.equal(status.abiVersion, 4);
    assert.ok(status.structSize > 0);
    for (const field of [
        'autoHwmPlannedSndHwmBytes',
        'autoHwmPlannedRcvHwmBytes',
        'autoHwmAppliedSndHwmBytes',
        'autoHwmAppliedRcvHwmBytes',
        'autoHwmDeferredSndHwmBytes',
        'autoHwmDeferredRcvHwmBytes',
        'sndBytesInFlight',
        'rcvBytesInFlight',
        'minimumCoreMessageChargeBytes',
        'oversizeMessageAdmissionMaxBytes',
        'flowPausedConnections',
        'flowPauseAppliedTotal',
        'flowResumeAppliedTotal',
        'flowStateStaleTotal',
        'flowPauseDurationMs'
    ]) {
        assert.equal(typeof status[field], 'bigint', field);
    }
    assert.equal(typeof status.sndPendingMsgs, 'bigint');
    assert.equal(typeof status.sndPendingBytes, 'bigint');
    assert.equal(typeof status.rcvPendingBytes, 'bigint');
    assert.equal(ctx.getCoreHwmBudgetSnapshot().monitorQueueAppliedHwmBytes, monitorHwmBytes * 2n);
    assert.throws(() => socket.monitorOpen(undefined, -1n), RangeError);
    assert.throws(() => socket.monitorOpen(undefined, 1n << 64n), RangeError);
    assert.throws(() => socket.monitorOpen(undefined, 4096), TypeError);
    monitor.close();
    socket.close();
    ctx.close();
});
