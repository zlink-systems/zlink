const assert = require('node:assert/strict');
const test = require('node:test');

process.env.NODE_ENV = 'test';
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkAutoConnectLoop
} = require('../../packages/framework/dist/runtime/locations/auto-connect-loop');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

test('Loop start and periodic ticks use the real Reconciler and LeaseTracker outside state turns', async () => {
  const store = new internal.ZLinkInMemoryLocationStore();
  const lease = await store.claimOwnerLease('owner', 30000);
  const tracker = new internal.ZLinkOwnerLeaseTracker({ store, options: { pollingIntervalMs: 0 } });
  assert.equal(await tracker.isOwnerLive('owner'), true);
  const local = {
    meshName: 'mesh',
    autoConnectType: internal.ZLinkLocationAutoConnectType.RouteMesh,
    role: internal.ZLinkLocationRole.Router,
    nodeRid: 'node',
    endpoint: 'tcp://local'
  };
  const reconciler = new internal.ZLinkAutoConnectReconciler({
    local,
    runtime: {},
    peerResolver: {
      async listLivePeers() {
        return [];
      }
    },
    executor: {
      connect() {
        assert.fail('no peer to connect');
      },
      disconnect() {}
    },
    errorSink: {
      reportRuntimeTaskException(task, error) {
        assert.fail(`${task}: ${error}`);
      }
    }
  });
  const loop = new ZLinkAutoConnectLoop({
    local,
    reconciler,
    leaseTracker: tracker,
    changeStampStore: {
      async getMeshNodeChangeStamp() {
        return 1n;
      }
    },
    setTimer: () => 1,
    clearTimer() {}
  });
  try {
    await loop.start();
    await loop.tick();
    await loop.requestPeerReconcile('mesh', 'peer');
    assert.equal(reconciler.storeFailed, false);
  } finally {
    await loop.stop();
    await store.releaseOwnerLease(lease.token);
  }
});

test('periodic and immediate scans share one operation order and stop discards queued reconnection', async () => {
  let stamp = 1n,
    scans = 0,
    active = 0,
    peak = 0,
    barrier,
    entered;
  const order = [];
  const loop = new ZLinkAutoConnectLoop({
    local: { meshName: 'mesh' },
    reconciler: {
      localPublicationReady: true,
      storeFailed: false,
      async tick() {
        const scan = ++scans;
        peak = Math.max(peak, ++active);
        order.push(`start:${scan}`);
        const held = barrier;
        entered?.resolve();
        try {
          if (held) await held.promise;
        } finally {
          order.push(`end:${scan}`);
          active--;
        }
      },
      disconnectPeers() {
        order.push('disconnect');
      },
      async unpublishLocal() {
        order.push('unpublish');
      }
    },
    changeStampStore: {
      async getMeshNodeChangeStamp() {
        return stamp;
      }
    },
    setTimer: () => 1,
    clearTimer() {}
  });
  await loop.start();
  try {
    await loop.tick();
    await loop.requestPeerReconcile('other', 'peer');
    assert.equal(scans, 1);
    stamp++;
    barrier = deferred();
    entered = deferred();
    const periodic = loop.tick();
    await entered.promise;
    const first = loop.requestPeerReconcile('mesh', 'peer-1');
    const second = loop.requestPeerReconcile('mesh', 'peer-2');
    assert.equal(scans, 2);
    barrier.resolve();
    await Promise.all([periodic, first, second]);
    assert.equal(
      scans,
      4,
      'both requests scan after the earlier periodic tick despite the same stamp'
    );
    assert.equal(peak, 1);
    assert.deepEqual(order, [
      'start:1',
      'end:1',
      'start:2',
      'end:2',
      'start:3',
      'end:3',
      'start:4',
      'end:4'
    ]);
    await loop.tick();
    assert.equal(scans, 4, 'periodic stamp optimization remains active');

    stamp++;
    barrier = deferred();
    entered = deferred();
    const running = loop.tick();
    await entered.promise;
    const queued = loop.requestPeerReconcile('mesh', 'queued');
    const stopping = loop.stop();
    barrier.resolve();
    await Promise.all([running, queued, stopping]);
    assert.equal(scans, 5, 'stop refuses a previously queued reconnect');
    assert.deepEqual(order.slice(-3), ['end:5', 'disconnect', 'unpublish']);
    await loop.requestPeerReconcile('mesh', 'stopped');
    assert.equal(scans, 5);
  } finally {
    barrier?.resolve();
    await loop.stop();
  }
});
