const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');

function barrier() {
  let resolve;
  const promise = new Promise((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

for (const weight of [0, 100]) {
  test(
    `local placement ignores stale store result: weight ${weight}`,
    { timeout: 10_000 },
    async () => {
      const host = new framework.ZLinkFrameworkRuntimeHost({
        registration: framework.createFrameworkRegistration({
          locations: { storeInstance: new framework.ZLinkInMemoryProviderLocationStore() },
          spotNodes: {
            play: {
              router: { bind: 'tcp://127.0.0.1:0' },
              objectRole: framework.ZLinkObjectRole.Server,
              actorLimit: 1,
              spotLimit: 1,
              meshChannels: { events: { server: true, weight: 100 } }
            }
          }
        })
      });
      const observation = new AbortController();
      const queryEntered = barrier();
      const releaseQuery = barrier();
      const writeEntered = barrier();
      const releaseWrite = barrier();
      let reconciliation;
      try {
        await host.start();
        const runtime = host.routeMeshRuntime;
        assert.equal(runtime.snapshot('play').placement.isAvailable, true);
        const existing = runtime.observe('play', 8, observation.signal)[Symbol.asyncIterator]();
        assert.equal((await existing.next()).value.status.placement.isAvailable, true);
        const location = host.locationOwner.currentRuntime;
        const manager = host.spotNodeRuntime;
        const list = location.listLiveMeshNodes.bind(location);
        // Hold the first query result after it has captured the old local descriptor.
        location.listLiveMeshNodes = async (...args) => {
          location.listLiveMeshNodes = list;
          const stale = await list(...args);
          assert.equal(stale[0].placementWeight, 100);
          queryEntered.resolve();
          await releaseQuery.promise;
          return stale;
        };
        reconciliation = manager.reconcileAndPublishMeshNodeState(
          framework.ZLinkFrameworkRuntimeState.Serving,
          'play'
        );
        await queryEntered.promise;
        host.routeMeshRuntimeOptions.mesh('play').placementWeight = weight;
        host.routeMeshRuntimeOptions.channel('events').weight = weight;
        await manager.waitForRuntimeWeightPublication();
        const beforeStaleResult = runtime.snapshot('play');
        assert.equal(beforeStaleResult.placement.isAvailable, weight > 0);
        if (weight === 0) {
          const changed = (await existing.next()).value.status;
          assert.equal(changed.placement.isAvailable, false);
          assert.equal(
            changed.placement.unavailableReason,
            framework.ZLinkTopologyReason.CapacityExceeded
          );
        }
        const write = location.writeMeshNode.bind(location);
        // Keep reconciliation's publication pending while observing the stale query result.
        location.writeMeshNode = async (...args) => {
          location.writeMeshNode = write;
          writeEntered.resolve();
          await releaseWrite.promise;
          return await write(...args);
        };
        releaseQuery.resolve();
        await writeEntered.promise;
        const assertPlacement = (status) => {
          assert.equal(status.placement.isAvailable, weight > 0);
          assert.deepEqual(status.channels, [
            {
              channelName: 'events',
              isReady: weight > 0,
              readyTargetCount: weight > 0 ? 1 : 0
            }
          ]);
          assert.equal(
            status.placement.unavailableReason,
            weight > 0 ? undefined : framework.ZLinkTopologyReason.CapacityExceeded
          );
        };
        const afterStaleResult = runtime.snapshot('play');
        assertPlacement(afterStaleResult);
        // An unchanged sequence leaves the registered observer's current status unchanged.
        assert.equal(afterStaleResult.sequence, beforeStaleResult.sequence);
        const current = runtime.observe('play', 8, observation.signal)[Symbol.asyncIterator]();
        const first = (await current.next()).value;
        assertPlacement(first.status);
        assert.deepEqual(first.loss, { coalescedCount: 0n, discardedTerminalCount: 0n });
        releaseWrite.resolve();
        await reconciliation;
        assertPlacement(runtime.snapshot('play'));
      } finally {
        releaseQuery.resolve();
        releaseWrite.resolve();
        await reconciliation;
        observation.abort();
        await host.stop();
      }
    }
  );
}
