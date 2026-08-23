// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const zlink = require('@zlink-systems/zlink');

test('send-completion callback queue does not keep the Node test worker alive', () => {
  const context = zlink.createContext();
  const socket = zlink.createPairSocket(context);

  socket.close();
  context.shutdown();
  context.close();
});

test('monitor callback queue does not keep the Node test worker alive', () => {
  const context = zlink.createContext();
  const socket = zlink.createPairSocket(context);
  const monitor = socket.monitorOpen();

  monitor.onEvent(() => {});

  monitor.close();
  socket.close();
  context.shutdown();
  context.close();
});
