// SPDX-License-Identifier: MPL-2.0

'use strict';

const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const { tcpEndpoint, waitForConnectionReady } = require('./sample_support');

function timeoutPromise(ms, label) {
  return new Promise((_, reject) => {
    setTimeout(() => reject(new Error(`${label} timed out`)), ms);
  });
}

async function main() {
// --8<-- [start:doc]
  const endpoint = await tcpEndpoint();
  const ctx = zlink.createContext();
  const routerSocket = zlink.createRouterSocket(ctx);
  const dealerSocket = zlink.createDealerSocket(ctx);
  const clientRoutingId = zlink.RoutingId.from(Buffer.from('request-reply-client'));

  try {
    const routerMonitor = routerSocket.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const dealerMonitor = dealerSocket.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    try {
      dealerSocket.setRoutingId(clientRoutingId);
      routerSocket.bind(endpoint);
      dealerSocket.connect(endpoint);
      await waitForConnectionReady(routerMonitor, zlink);
      await waitForConnectionReady(dealerMonitor, zlink);
    } finally {
      routerMonitor.close();
      dealerMonitor.close();
    }

    const pendingReply = dealerSocket.request()
      .message(Buffer.from('ping'))
      .timeout(2000)
      .submit();
    const request = new zlink.Received();
    routerSocket.recv(request);
    try {
      assert.equal(request.routingId.toBytes().toString(), 'request-reply-client');
      assert.ok(typeof request.requestSeq === 'bigint');
      routerSocket.reply(request.routingId, request.requestSeq)
        .message(Buffer.from('pong'))
        .submit();
    } finally {
      request.close();
    }
    const reply = await pendingReply;
    try {
      assert.equal(reply[0].data().toString(), 'pong');
    } finally {
      for (const part of reply) {
        part.close();
      }
    }
    console.log('[dealer-router/request-reply] send: "ping" -> recv: "pong"');
  } finally {
    dealerSocket.close();
    routerSocket.close();
    ctx.close();
  }
// --8<-- [end:doc]
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
