// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const zlink = require('@zlink-systems/zlink');

test('pull completion owner does not keep an idle Node worker alive', () => {
  const context = zlink.createContext();
  const socket = zlink.createPairSocket(context);

  socket.close();
  context.shutdown();
  context.close();
});

test('monitor and timer are pull-only', () => {
  const context = zlink.createContext();
  const socket = zlink.createPairSocket(context);
  const monitor = socket.monitorOpen();

  const timer = zlink.createTimer();
  require('node:assert/strict').equal(monitor.onEvent, undefined);
  require('node:assert/strict').equal(timer.onFire, undefined);

  timer.close();
  monitor.close();
  socket.close();
  context.shutdown();
  context.close();
});
