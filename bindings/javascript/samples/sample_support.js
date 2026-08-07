// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');

async function waitForConnectionReady(monitor, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const event = monitor.recv(zlink.RecvFlags.DontWait);
    if (event && event.event === zlink.MonitorEventType.ConnectionReady) {
      return event;
    }
    if (monitor.status().isReady()) {
      return null;
    }
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
  throw new Error('connection-ready monitor event timed out');
}

module.exports = { waitForConnectionReady };
