// SPDX-License-Identifier: MPL-2.0

'use strict';

const assert = require('node:assert/strict');
const { once } = require('node:events');
const net = require('node:net');
const zlink = require('@zlink-systems/zlink');

async function reservePort() {
  const srv = net.createServer();
  srv.listen(0, '127.0.0.1');
  await once(srv, 'listening');
  const { port } = srv.address();
  await new Promise((resolve, reject) => srv.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function main() {
// --8<-- [start:doc]
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const ctx = zlink.createContext();
  const pub = zlink.createPubSocket(ctx);
  const sub = zlink.createSubSocket(ctx);

  try {
    const pubMonitor = pub.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    const subMonitor = sub.monitorOpen([zlink.MonitorEventType.ConnectionReady]);
    try {
      pub.bind(endpoint);
      sub.connect(endpoint);
      pubMonitor.recv();
      subMonitor.recv();
    } finally {
      pubMonitor.close();
      subMonitor.close();
    }

    const topic = 'prices';
    const sent = '101.25';
    sub.setSubscription(topic);
    const deadline = Date.now() + 5000;
    const received = new zlink.TopicMessage();
    let hasReceived = false;
    while (Date.now() < deadline) {
      pub.publish(topic).message(Buffer.from(sent)).submit();
      try {
        if (sub.subscribe(received, zlink.RecvFlags.DontWait)) {
          hasReceived = true;
          break;
        }
      } catch (error) {
        if (!(error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData)) {
          throw error;
        }
      }
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    assert.equal(hasReceived, true);
    try {
      const recv = received.parts[0].data().toString();
      assert.equal(received.topic, topic);
      assert.equal(recv, sent);
      console.log(`[pubsub/recv] publish: "${topic}/${sent}" → subscribe: "${topic}/${recv}"`);
    } finally {
      received.close();
    }
  } finally {
    sub.close();
    pub.close();
    ctx.close();
  }
// --8<-- [end:doc]
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
