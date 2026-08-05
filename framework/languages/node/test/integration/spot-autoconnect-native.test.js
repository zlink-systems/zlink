const assert = require('node:assert/strict');
const net = require('node:net');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');

test('single connectPeerRid plus bilateral peer subscriptions supports requests in both directions', async () => {
  const endpoints = await reservePorts(4);
  const aContext = zlink.createContext();
  const bContext = zlink.createContext();
  const aNode = zlink.createSpotNode(aContext);
  const bNode = zlink.createSpotNode(bContext);
  const aSpot = aNode.createSpot();
  const bSpot = bNode.createSpot();
  try {
    const aNodeRid = routingId('AUTO_NODE_A');
    const bNodeRid = routingId('AUTO_NODE_B');
    aNode.setRoutingId(aNodeRid);
    bNode.setRoutingId(bNodeRid);
    aSpot.setRoutingId(routingId('AUTO_SPOT_A'));
    bSpot.setRoutingId(routingId('AUTO_SPOT_B'));
    aNode.setRouterBind(endpoints[0]);
    bNode.setRouterBind(endpoints[1]);
    aNode.setPubBind(endpoints[2]);
    bNode.setPubBind(endpoints[3]);
    aNode.connectPeerRid(bNodeRid, endpoints[1]);
    aNode.connectPeer(endpoints[3]);
    bNode.connectPeer(endpoints[2]);
    await Promise.all([waitForPeer(aNode), waitForPeer(bNode)]);

    await requestAndReply(aSpot, bSpot, bNodeRid, bSpot.routingId, 'a-to-b');
    await requestAndReply(bSpot, aSpot, aNodeRid, aSpot.routingId, 'b-to-a');
  } finally {
    bSpot.close();
    aSpot.close();
    bNode.close();
    aNode.close();
    bContext.close();
    aContext.close();
  }
});

test('single connectPeerRid supports reverse requests between entry spots', async () => {
  const endpoints = await reservePorts(4);
  const aContext = zlink.createContext();
  const bContext = zlink.createContext();
  const aNode = zlink.createSpotNode(aContext);
  const bNode = zlink.createSpotNode(bContext);
  const aSpot = aNode.entrySpot();
  const bSpot = bNode.entrySpot();
  try {
    const aNodeRid = routingId('ENTRY_NODE_A');
    const bNodeRid = routingId('ENTRY_NODE_B');
    aNode.setRoutingId(aNodeRid);
    bNode.setRoutingId(bNodeRid);
    aSpot.setRoutingId(aNodeRid);
    bSpot.setRoutingId(bNodeRid);
    aNode.setRouterBind(endpoints[0]);
    bNode.setRouterBind(endpoints[1]);
    aNode.setPubBind(endpoints[2]);
    bNode.setPubBind(endpoints[3]);
    aNode.connectPeerRid(bNodeRid, endpoints[1]);
    aNode.connectPeer(endpoints[3]);
    bNode.connectPeer(endpoints[2]);
    await Promise.all([waitForPeer(aNode), waitForPeer(bNode)]);

    await requestAndReplyCallback(aSpot, bSpot, bNodeRid, bNodeRid, 'entry-a-to-b');
    await requestAndReplyCallback(bSpot, aSpot, aNodeRid, aNodeRid, 'entry-b-to-a');
  } finally {
    bNode.close();
    aNode.close();
    bContext.close();
    aContext.close();
  }
});

test('single connectPeerRid supports router-only requests between entry spots in both directions', async () => {
  const endpoints = await reservePorts(2);
  const aContext = zlink.createContext();
  const bContext = zlink.createContext();
  const aNode = zlink.createSpotNode(aContext, zlink.SpotNodeMode.Routed);
  const bNode = zlink.createSpotNode(bContext, zlink.SpotNodeMode.Routed);
  const aSpot = aNode.entrySpot();
  const bSpot = bNode.entrySpot();
  const aTarget = aNode.createSpot();
  const bTarget = bNode.createSpot();
  try {
    const aNodeRid = routingId('ROUTER_NODE_A');
    const bNodeRid = routingId('ROUTER_NODE_B');
    aNode.setRoutingId(aNodeRid);
    bNode.setRoutingId(bNodeRid);
    aSpot.setRoutingId(aNodeRid);
    bSpot.setRoutingId(bNodeRid);
    aTarget.setRoutingId(routingId('ROUTER_SPOT_A'));
    bTarget.setRoutingId(routingId('ROUTER_SPOT_B'));
    aNode.setRouterBind(endpoints[0]);
    bNode.setRouterBind(endpoints[1]);
    aNode.connectPeerRid(bNodeRid, endpoints[1]);
    await requestAndReplyCallback(bSpot, aTarget, aNodeRid, aTarget.routingId, 'router-b-to-a');
    await requestAndReplyCallback(aSpot, bTarget, bNodeRid, bTarget.routingId, 'router-a-to-b');
  } finally {
    bTarget.close();
    aTarget.close();
    bNode.close();
    aNode.close();
    bContext.close();
    aContext.close();
  }
});

test('connectPeerRid replaces an outbound route when the same rid moves to a new endpoint', async () => {
  const endpoints = await reservePorts(3);
  const contexts = Array.from({ length: 3 }, () => zlink.createContext());
  const aNode = zlink.createSpotNode(contexts[0], zlink.SpotNodeMode.Routed);
  const oldNode = zlink.createSpotNode(contexts[1], zlink.SpotNodeMode.Routed);
  let replacementNode;
  try {
    const aRid = routingId('REPLACE_A');
    const peerRid = routingId('REPLACE_B');
    const aEntry = aNode.entrySpot();
    const oldEntry = oldNode.entrySpot();
    aNode.setRoutingId(aRid);
    aEntry.setRoutingId(aRid);
    oldNode.setRoutingId(peerRid);
    oldEntry.setRoutingId(peerRid);
    aNode.setRouterBind(endpoints[0]);
    oldNode.setRouterBind(endpoints[1]);
    aNode.connectPeerRid(peerRid, endpoints[1]);
    await requestAndReplyCallback(aEntry, oldEntry, peerRid, peerRid, 'old-route');

    oldNode.close();
    aNode.disconnectPeerRid(peerRid);
    await waitForPeerEndpointDisconnected(aNode, endpoints[1]);
    replacementNode = zlink.createSpotNode(contexts[2], zlink.SpotNodeMode.Routed);
    const replacementEntry = replacementNode.entrySpot();
    replacementNode.setRoutingId(peerRid);
    replacementEntry.setRoutingId(peerRid);
    replacementNode.setRouterBind(endpoints[2]);
    aNode.connectPeerRid(peerRid, endpoints[2]);
    await requestAndReplyCallback(aEntry, replacementEntry, peerRid, peerRid, 'replacement-route');
  } finally {
    replacementNode?.close();
    aNode.close();
    contexts.slice().reverse().forEach((context) => context.close());
  }
});

test('one initiator keeps reverse-first router-only requests reachable across multiple peers', async () => {
  const endpoints = await reservePorts(6);
  const contexts = Array.from({ length: 4 }, () => zlink.createContext());
  const nodes = contexts.map((context, index) => index < 2
    ? zlink.createSpotNode(context)
    : zlink.createSpotNode(context, zlink.SpotNodeMode.Routed));
  const entries = nodes.map((node) => node.entrySpot());
  const target = nodes[0].createSpot();
  try {
    const nodeRids = [
      routingId('bingo-play-node-a'),
      routingId('bingo-play-node-b'),
      routingId('bingo-session-node-a'),
      routingId('bingo-session-node-b')
    ];
    nodes.forEach((node, index) => {
      node.setRoutingId(nodeRids[index]);
      entries[index].setRoutingId(nodeRids[index]);
      node.setRouterBind(endpoints[index]);
    });
    nodes[0].setPubBind(endpoints[4]);
    nodes[1].setPubBind(endpoints[5]);
    target.setRoutingId(routingId('bingo-room-regression'));
    nodes[0].connectPeerRid(nodeRids[1], endpoints[1]);
    nodes[0].connectPeer(endpoints[5]);
    nodes[0].connectPeerRid(nodeRids[2], endpoints[2]);
    nodes[0].connectPeerRid(nodeRids[3], endpoints[3]);
    nodes[1].connectPeer(endpoints[4]);
    nodes[1].connectPeerRid(nodeRids[2], endpoints[2]);
    nodes[1].connectPeerRid(nodeRids[3], endpoints[3]);
    nodes[2].connectPeer(endpoints[4]);
    nodes[2].connectPeer(endpoints[5]);
    nodes[2].connectPeerRid(nodeRids[3], endpoints[3]);
    nodes[3].connectPeer(endpoints[4]);
    nodes[3].connectPeer(endpoints[5]);

    await requestAndReplyCallback(
      entries[2], target, nodeRids[0], target.routingId, 'session-a-to-play-a');
    await requestAndReplyCallback(
      target, entries[2], nodeRids[2], nodeRids[2], 'play-a-to-session-a');
  } finally {
    target.close();
    nodes.slice().reverse().forEach((node) => node.close());
    contexts.slice().reverse().forEach((context) => context.close());
  }
});

test('reverse-first router-only request permits a nested request before the outer reply', async () => {
  const endpoints = await reservePorts(2);
  const aContext = zlink.createContext();
  const bContext = zlink.createContext();
  const aNode = zlink.createSpotNode(aContext, zlink.SpotNodeMode.Routed);
  const bNode = zlink.createSpotNode(bContext, zlink.SpotNodeMode.Routed);
  const aTarget = aNode.createSpot();
  const bEntry = bNode.entrySpot();
  try {
    const aNodeRid = routingId('bingo-play-node-a');
    const bNodeRid = routingId('bingo-session-node-a');
    aNode.setRoutingId(aNodeRid);
    bNode.setRoutingId(bNodeRid);
    aTarget.setRoutingId(routingId('bingo-player-1'));
    bEntry.setRoutingId(bNodeRid);
    aNode.setRouterBind(endpoints[0]);
    bNode.setRouterBind(endpoints[1]);
    aNode.connectPeerRid(bNodeRid, endpoints[1]);

    const outerReceived = new Promise((resolve, reject) => {
      aTarget.setDispatchHandler((info) => {
        if (info.event !== zlink.SpotDispatchEvent.RoutedReadable || info.routed === null) return;
        const outer = info.routed;
        const accepted = aTarget.requestToSpot(bNodeRid, bNodeRid)
          .message(Buffer.from('nested-response-control'))
          .timeout(3000)
          .submit((result, parts) => {
            try {
              assert.equal(result, 0);
              parts.forEach((part) => part.close());
              outer.reply().message(Buffer.from('outer-reply')).submit();
              outer.close();
              resolve();
            } catch (error) {
              reject(error);
            }
          });
        if (!accepted) reject(new Error('nested request was not accepted'));
      });
    });
    bEntry.setDispatchHandler((info) => {
      if (info.event !== zlink.SpotDispatchEvent.RoutedReadable || info.routed === null) return;
      info.routed.reply().message(Buffer.from('nested-ack')).submit();
      info.routed.close();
    });

    const outerReply = await new Promise((resolve, reject) => {
      const accepted = bEntry.requestToSpot(aNodeRid, aTarget.routingId)
        .message(Buffer.from('outer-actor-request'))
        .timeout(5000)
        .submit((result, parts) => result === 0 ? resolve(parts) : reject(new Error(`outer request result ${result}`)));
      if (!accepted) reject(new Error('outer request was not accepted'));
    });
    outerReply.forEach((part) => part.close());
    await outerReceived;
  } finally {
    aTarget.close();
    bNode.close();
    aNode.close();
    bContext.close();
    aContext.close();
  }
});

test('single connectPeerRid preserves multipart requests on the reverse learned route', async () => {
  const endpoints = await reservePorts(2);
  const aContext = zlink.createContext();
  const bContext = zlink.createContext();
  const aNode = zlink.createSpotNode(aContext, zlink.SpotNodeMode.Routed);
  const bNode = zlink.createSpotNode(bContext, zlink.SpotNodeMode.Routed);
  const bSource = bNode.entrySpot();
  const aTarget = aNode.createSpot();
  try {
    const aNodeRid = routingId('MULTIPART_NODE_A');
    const bNodeRid = routingId('MULTIPART_NODE_B');
    aNode.setRoutingId(aNodeRid);
    bNode.setRoutingId(bNodeRid);
    bSource.setRoutingId(bNodeRid);
    aTarget.setRoutingId(routingId('MULTIPART_SPOT_A'));
    aNode.setRouterBind(endpoints[0]);
    bNode.setRouterBind(endpoints[1]);
    aNode.connectPeerRid(bNodeRid, endpoints[1]);

    const handled = receiveMultipartAndReply(aTarget, ['channel-header', 'payload']);
    const reply = await new Promise((resolve, reject) => {
      const accepted = bSource.requestToSpot(aNodeRid, aTarget.routingId)
        .message(Buffer.from('channel-header'))
        .message(Buffer.from('payload'))
        .timeout(3000)
        .submit((result, parts) => result === 0 ? resolve(parts) : reject(new Error(`request result ${result}`)));
      if (!accepted) reject(new Error('multipart request was not accepted'));
    });
    try {
      assert.equal(reply[0].data().toString(), 'multipart-reply');
    } finally {
      reply.forEach((part) => part.close());
    }
    await handled;
  } finally {
    aTarget.close();
    bNode.close();
    aNode.close();
    bContext.close();
    aContext.close();
  }
});

async function requestAndReplyCallback(sender, receiver, targetNodeRid, targetSpotId, text) {
  const handled = receiveAndReply(receiver, text);
  const reply = await new Promise((resolve, reject) => {
    const accepted = sender.requestToSpot(targetNodeRid, targetSpotId)
      .message(Buffer.from(text))
      .timeout(3000)
      .submit((result, parts) => result === 0 ? resolve(parts) : reject(new Error(`request result ${result}`)));
    if (!accepted) reject(new Error('request was not accepted'));
  });
  try {
    assert.equal(reply[0].data().toString(), `${text}-reply`);
  } finally {
    reply.forEach((part) => part.close());
  }
  await handled;
}

async function sendAndReceive(sender, receiver, targetNodeRid, targetSpotId, text) {
  const received = receiveOne(receiver, text);
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (sender.sendToSpot(targetNodeRid, targetSpotId).message(Buffer.from(text)).submit()) {
      await received;
      return;
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  await assert.rejects(received);
  throw new Error(`timed out sending '${text}'`);
}

async function receiveOne(spot, text) {
  const received = new zlink.Received();
  const deadline = Date.now() + 5000;
  try {
    while (Date.now() < deadline) {
      if (spot.recvRouted(received, zlink.RecvFlags.DontWait)) {
        assert.equal(received.parts[0].data().toString(), text);
        return;
      }
      await new Promise((resolve) => setImmediate(resolve));
    }
    throw new Error(`timed out receiving '${text}'`);
  } finally {
    received.close();
  }
}

async function requestAndReply(sender, receiver, targetNodeRid, targetSpotId, text) {
  const handled = receiveAndReply(receiver, text);
  const reply = await sender.requestToSpot(targetNodeRid, targetSpotId)
    .message(Buffer.from(text))
    .timeout(3000)
    .submit();
  try {
    assert.equal(reply[0].data().toString(), `${text}-reply`);
  } finally {
    reply.forEach((part) => part.close());
  }
  await handled;
}

async function receiveAndReply(spot, text) {
  const received = new zlink.Received();
  const deadline = Date.now() + 5000;
  try {
    while (Date.now() < deadline) {
      if (spot.recvRouted(received, zlink.RecvFlags.DontWait)) {
        assert.equal(received.parts[0].data().toString(), text);
        received.reply().message(Buffer.from(`${text}-reply`)).submit();
        return;
      }
      await new Promise((resolve) => setImmediate(resolve));
    }
    throw new Error(`timed out receiving '${text}'`);
  } finally {
    received.close();
  }
}

async function receiveMultipartAndReply(spot, expectedParts) {
  const received = new zlink.Received();
  const deadline = Date.now() + 5000;
  try {
    while (Date.now() < deadline) {
      if (spot.recvRouted(received, zlink.RecvFlags.DontWait)) {
        assert.deepEqual(received.parts.map((part) => part.data().toString()), expectedParts);
        received.reply().message(Buffer.from('multipart-reply')).submit();
        return;
      }
      await new Promise((resolve) => setImmediate(resolve));
    }
    throw new Error('timed out receiving multipart request');
  } finally {
    received.close();
  }
}

async function waitForPeer(node) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (node.status().connectedPeerCount > 0) return;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  throw new Error('spot peer readiness timed out');
}

async function waitForPeerEndpointDisconnected(node, endpoint) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (!node.peers().some((peer) => peer.peerEndpoint === endpoint && peer.state === 3)) return;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  throw new Error(`spot peer '${endpoint}' disconnect timed out`);
}

async function reservePorts(count) {
  const servers = [];
  const endpoints = [];
  try {
    for (let i = 0; i < count; i++) {
      const server = net.createServer();
      servers.push(server);
      server.listen(0, '127.0.0.1');
      await new Promise((resolve, reject) => {
        server.once('listening', resolve);
        server.once('error', reject);
      });
      endpoints.push(`tcp://127.0.0.1:${server.address().port}`);
    }
  } finally {
    await Promise.all(servers.map((server) => new Promise((resolve, reject) => {
      server.close((error) => error ? reject(error) : resolve());
    })));
  }
  return endpoints;
}

function routingId(text) {
  return zlink.RoutingId.from(Buffer.from(text, 'ascii'));
}
