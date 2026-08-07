// SPDX-License-Identifier: MPL-2.0

'use strict';

const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const { tcpEndpoint, waitForConnectionReady } = require('./sample_support');

async function main() {
// --8<-- [start:doc]
  const endpoint = await tcpEndpoint();
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);

  try {
    const routerMonitor = router.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const dealerMonitor = dealer.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    try {
      router.bind(endpoint);
      dealer.connect(endpoint);
      await waitForConnectionReady(routerMonitor, zlink);
      await waitForConnectionReady(dealerMonitor, zlink);
    } finally {
      routerMonitor.close();
      dealerMonitor.close();
    }

    const sent = 'ping';
    dealer.send().message(Buffer.from(sent)).submit();

    const reply = 'pong';
    const request = new zlink.Received();
    router.recv(request);
    try {
      const recvReq = request.parts[0].data().toString();
      assert.equal(recvReq, sent);
      assert.ok(request.routingId instanceof zlink.RoutingId);
      request.send().message(Buffer.from(reply)).submit();
    } finally {
      request.close();
    }

    const response = new zlink.Received();
    dealer.recv(response);
    try {
      const recv = response.parts[0].data().toString();
      assert.equal(recv, reply);
      console.log(`[dealer-router/recv] send: "${sent}" \u2192 recv: "${recv}"`);
    } finally {
      response.close();
    }
  } finally {
    dealer.close();
    router.close();
    ctx.close();
  }
// --8<-- [end:doc]
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
