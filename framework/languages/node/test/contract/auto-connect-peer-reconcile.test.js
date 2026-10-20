const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkAutoConnectLoop } = require('../../packages/framework/dist/runtime/locations/auto-connect-loop');
function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}
test('immediate peer reconcile bypasses polling stamp and shares the periodic owner lane', async () => {
  let ticks = 0, concurrent = 0, peak = 0, held, entered;
  const loop = new ZLinkAutoConnectLoop({
    local: { meshName: 'mesh' },
    reconciler: {
      localPublicationReady: true, storeFailed: false,
      async tick() {
        ticks++;
        peak = Math.max(peak, ++concurrent);
        const barrier = held;
        entered?.resolve();
        if (barrier) await barrier.promise;
        concurrent--;
      },
      disconnectPeers() {}, async unpublishLocal() {}
    },
    changeStampStore: { async getMeshNodeChangeStamp() { return 1n; } },
    setTimer: () => 1, clearTimer() {}
  });
  await loop.start();
  try {
    await loop.tick();
    assert.equal(ticks, 1, 'unchanged stamp skips periodic reconciliation');
    await loop.requestPeerReconcile('other-mesh', 'peer');
    assert.equal(ticks, 1);
    held = deferred(); entered = deferred();
    const immediate = loop.requestPeerReconcile('mesh', 'peer');
    await entered.promise;
    const queued = loop.requestPeerReconcile('mesh', 'other-peer');
    assert.equal(ticks, 2);
    held.resolve();
    await Promise.all([immediate, queued]);
    assert.equal(ticks, 3);
    assert.equal(peak, 1);
  } finally { held?.resolve(); await loop.stop(); }
  await loop.requestPeerReconcile('mesh', 'peer');
  assert.equal(ticks, 3, 'a stopped owner cannot reconnect');
});
