const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const {
  ServiceLivenessRegistry
} = require('../../packages/framework/dist/runtime/foundation/service-liveness-registry');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');

test('transport liveness section 5 makes an admitted handshake Ready before any ACK', () => {
  // 05-transport-liveness.ko.md:228: Ready has no probe-ACK prerequisite.
  const liveness = new ServiceLivenessRegistry();
  liveness.admit('peer', 'pair', 0);
  const runtime = {
    topology: { peer: () => ({ connectionId: 'pair', descriptor: { lifecycleGeneration: 7n } }) },
    liveness
  };
  assert.equal(RawServiceMeshRuntime.prototype.isPeerRouteReady.call(runtime, 'peer', 7n), true);
  assert.equal(RawServiceMeshRuntime.prototype.isPeerRouteReady.call(runtime, 'peer', 8n), false);
});

test('any-record owner matches the shared four-language contract', () => {
  const fixture = path.resolve(__dirname, '../../../../test/fixtures/liveness-any-record.tsv');
  const rows = fs
    .readFileSync(fixture, 'utf8')
    .split(/\r?\n/)
    .filter((row) => row && !row.startsWith('#'));
  assert.equal(rows.length, 9);
  for (const row of rows) {
    const [kind, received, alive, expires, expectedProbe] = row.split('\t');
    const owner = new ServiceLivenessRegistry();
    let current = owner.admit('peer', 'current', 0);
    owner.tick(5_000);
    let receipt = current;
    if (kind === 'retired_connection') current = owner.admit('peer', 'replacement', 6_000);
    else if (kind === 'other_connection') receipt = owner.admit('other', 'other', 0);
    if (kind === 'previous_ack') owner.acknowledge('peer', 'current', 2n, Number(received));
    else owner.recordReceived(receipt, Number(received));
    const tick = owner.tick(Number(alive));
    assert.equal(tick.timedOutNodes.includes('peer'), false, kind);
    assert.equal(
      tick.probes.filter((probe) => probe.nodeRoutingId === 'peer').length,
      Number(expectedProbe),
      kind
    );
    assert.equal(owner.tick(Number(expires)).timedOutNodes.includes('peer'), true, kind);
  }
});

test('transport liveness section 3 makes the first probe due at admission', () => {
  const owner = new ServiceLivenessRegistry();
  owner.admit('peer', 'pair', 100);
  assert.equal(owner.tick(100).probes.length, 1);
  assert.equal(owner.tick(5_099).probes.length, 0);
  assert.equal(owner.tick(5_100).probes.length, 1);
});

test('ClientServer admission attaches the owner to its existing physical connection', () => {
  const {
    ZLinkChannelSocketRegistry
  } = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
  const connection = { dealer: {}, aliases: new Set(['connection']), callbacksByAlias: new Map() };
  const sockets = {
    clientServerConnections: new Map([['connection', connection]]),
    clientServerReadyIdentities: new Map(),
    clientServerDiscovery: { admitClientServer: () => true },
    notifyClientServerTopology: () => {}
  };
  const descriptor = { channelName: 'orders', serverRoutingId: 'server', lifecycleGeneration: 7n };
  assert.equal(
    ZLinkChannelSocketRegistry.prototype.admitClientServerConnection.call(
      sockets,
      descriptor,
      'connection'
    ),
    true
  );
  assert.ok(connection.liveness);
  const owner = connection.liveness;
  assert.equal(
    ZLinkChannelSocketRegistry.prototype.admitClientServerConnection.call(
      sockets,
      descriptor,
      'connection'
    ),
    true
  );
  assert.equal(connection.liveness, owner);
});

test('duplicate and previous ACK records extend the current connection deadline', () => {
  const owner = new ServiceLivenessRegistry();
  owner.admit('peer', 'pair', 0);
  const probe = owner.tick(5_000).probes[0].probeId;
  assert.equal(owner.acknowledge('peer', 'pair', probe + 1n, 14_000), false);
  assert.deepEqual(owner.tick(15_000).timedOutNodes, []);
  assert.equal(owner.tick(20_000).probes[0].probeId, probe);
  assert.deepEqual(owner.tick(29_000).timedOutNodes, ['peer']);
});

test('a retired connection ACK cannot extend its replacement or another peer', () => {
  const owner = new ServiceLivenessRegistry();
  owner.admit('peer', 'old', 0);
  const probe = owner.tick(5_000).probes[0].probeId;
  owner.admit('peer', 'current', 6_000);
  owner.admit('other', 'other-pair', 6_000);
  assert.equal(owner.acknowledge('peer', 'old', probe, 20_000), false);
  assert.deepEqual(owner.tick(21_000).timedOutNodes, ['other', 'peer']);
});

test('ClientServer server ordinary receive refreshes only its peer and expires after the last record', async () => {
  const {
    ZLinkChannelSocketRegistry
  } = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
  const { Message, RoutingId } = require('@zlink-systems/zlink');
  const sockets = Object.create(ZLinkChannelSocketRegistry.prototype);
  const disconnected = [];
  const router = {
    async send() {},
    disconnectPeer(rid) {
      disconnected.push(rid);
    }
  };
  Object.assign(sockets, {
    clientServerConnections: new Map(),
    clientServerServerPeers: new Map(),
    clientServerAdmittedClients: new Map(),
    channelRouters: new Map([['orders', router]]),
    fanoutConnections: new Map(),
    ownedResources: [],
    publishers: new Map(),
    nextClientServerProbeId: 1n,
    ensureClientServerLivenessTimer() {}
  });
  sockets.admitClientServerServerPeer('orders', 'active', 4096);
  sockets.admitClientServerServerPeer('orders', 'silent', 4096);
  const beforeReceive = performance.now();
  const message = Message.from('ordinary-message');
  try {
    assert.equal(
      sockets.tryHandleClientServerControl(
        'orders',
        {
          parts: [message],
          routingId: RoutingId.from('active'),
          replyToken: null
        },
        router
      ),
      false
    );
  } finally {
    message.close();
  }
  const afterReceive = performance.now();
  await sockets.tickClientServerLiveness(beforeReceive + 15_000);
  assert.deepEqual(disconnected, ['silent']);
  await sockets.tickClientServerLiveness(afterReceive + 15_000);
  assert.deepEqual(disconnected, ['silent', 'active']);
});

test('ClientServer server receive from another routing id does not refresh this peer', async () => {
  const {
    ZLinkChannelSocketRegistry
  } = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
  const { Message, RoutingId } = require('@zlink-systems/zlink');
  const sockets = Object.create(ZLinkChannelSocketRegistry.prototype);
  const disconnected = [];
  const router = {
    async send() {},
    disconnectPeer(rid) {
      disconnected.push(rid);
    }
  };
  Object.assign(sockets, {
    clientServerConnections: new Map(),
    clientServerServerPeers: new Map(),
    clientServerAdmittedClients: new Map(),
    channelRouters: new Map([['orders', router]]),
    fanoutConnections: new Map(),
    ownedResources: [],
    publishers: new Map(),
    nextClientServerProbeId: 1n,
    ensureClientServerLivenessTimer() {}
  });
  sockets.admitClientServerServerPeer('orders', 'active', 4096);
  sockets.admitClientServerServerPeer('orders', 'other', 4096);
  const peers = sockets.clientServerServerPeersForChannel('orders');
  const beforeReceive = performance.now();
  peers.get('active').liveness.deadlineMs = beforeReceive + 1_000;
  peers.get('other').liveness.deadlineMs = beforeReceive + 1_000;
  const message = Message.from('ordinary-message');
  try {
    assert.equal(
      sockets.tryHandleClientServerControl(
        'orders',
        {
          parts: [message],
          routingId: RoutingId.from('other'),
          replyToken: null
        },
        router
      ),
      false
    );
  } finally {
    message.close();
  }
  const afterReceive = performance.now();
  await sockets.tickClientServerLiveness(beforeReceive + 2_000);
  assert.deepEqual(disconnected, ['active']);
  await sockets.tickClientServerLiveness(afterReceive + 15_000);
  assert.deepEqual(disconnected, ['active', 'other']);
});
