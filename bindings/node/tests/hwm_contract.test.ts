'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

test('HWM and Auto HWM planning unit preserve uint64 byte values', () => {
  const ctx = zlink.createContext();
  ctx.options.autoHwmEnabled = false;
  const socket = zlink.createPairSocket(ctx);
  const maxPlanningUnit = ((1n << 64n) - 1n) / 512n;
  const maxHwm = (1n << 64n) - 1n;

  assert.equal(socket.options.sendHwm, 4_096_000n);
  assert.equal(socket.options.recvHwm, 4_096_000n);

  ctx.options.autoHwmMsgUnitBytes = maxPlanningUnit;
  assert.equal(ctx.options.autoHwmMsgUnitBytes, maxPlanningUnit);

  socket.options.sendHwm = maxHwm;
  socket.options.recvHwm = 0n;
  assert.equal(socket.options.sendHwm, maxHwm);
  assert.equal(socket.options.recvHwm, 0n);

  assert.throws(() => { socket.options.sendHwm = 4096; }, TypeError);
  assert.throws(() => { ctx.options.autoHwmMsgUnitBytes = 4096; }, TypeError);
  assert.throws(() => { socket.options.recvHwm = -1n; }, RangeError);

  socket.close();
  ctx.close();
});

test('monitor ABI v2 exposes byte telemetry as bigint', () => {
  const ctx = zlink.createContext();
  const socket = zlink.createPairSocket(ctx);
  const monitor = socket.monitorOpen();
  const status = monitor.status();

  assert.equal(status.abiVersion, 2);
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
    'oversizeMessageAdmissionMaxBytes'
  ]) {
    assert.equal(typeof status[field], 'bigint', field);
  }
  assert.equal(typeof status.sndPendingMsgs, 'bigint');
  assert.equal(typeof status.autoHwmSocketMessageSlots, 'bigint');

  monitor.close();
  socket.close();
  ctx.close();
});
