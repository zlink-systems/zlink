const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const internal = require('../../packages/framework/dist/internal');
const {
  spotNodeAutoConnectCapability
} = require('../../packages/framework/dist/runtime/spots/spot-node-autoconnect');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

for (const snapshot of ['absent', 'previous lifecycle']) {
  test(`an older discovery snapshot preserves a later admitted passive Session owner: ${snapshot}`, async () => {
    const calls = [];
    const remoteRid = zlink.RoutingId.from('node-remote');
    const readEntered = deferred();
    const releaseRead = deferred();
    let admitted = false;
    const node = {
      status() {
        return { routingId: zlink.RoutingId.from('node-local'), localEndpoint: 'tcp://local' };
      },
      hasPeerRouteOrConnectionIntent() {
        return admitted;
      },
      peers() {
        return admitted
          ? [{ endpoint: 'tcp://remote', routingId: remoteRid, lifecycleGeneration: 17n, state: 3 }]
          : [];
      },
      disconnectPeer(peerRid, lifecycleGeneration) {
        calls.push(`disconnect:${peerRid}:${lifecycleGeneration}`);
        admitted = false;
      }
    };
    const capability = spotNodeAutoConnectCapability(
      'play',
      { router: { bind: 'tcp://local' } },
      node
    );
    const rows =
      snapshot === 'absent'
        ? []
        : [
            {
              autoConnectType: internal.ZLinkLocationAutoConnectType.RouteMesh,
              meshName: 'play',
              nodeRid: remoteRid,
              generation: 18n,
              endpoint: 'tcp://remote',
              role: internal.ZLinkLocationRole.Router,
              ownerId: 'session-owner',
              weight: 100,
              draining: false,
              value: 1n,
              updatedAt: new Date(0)
            }
          ];
    const reconciler = new internal.ZLinkAutoConnectReconciler({
      local: capability.local,
      runtime: {},
      peerResolver: {
        async listLivePeers() {
          readEntered.resolve();
          await releaseRead.promise;
          return rows;
        }
      },
      executor: capability.executor,
      errorSink: {
        reportRuntimeTaskException(task, error) {
          assert.fail(`${task}: ${error}`);
        }
      }
    });
    const tick = reconciler.tick();
    try {
      await readEntered.promise;
      // The Store read precedes transport admission and command 38. Its
      // result cannot decide the lifetime of that subsequently admitted route
      // (transport-liveness §5; Session–Actor binding §8.1).
      admitted = true;
      releaseRead.resolve();
      await tick;
      assert.equal(admitted, true, 'transport liveness retains the admitted Session route');
      assert.deepEqual(calls, [], 'discovery owns no connection intent for this passive peer');
      assert.equal(reconciler.activeTargets.length, 0, 'no second connection intent is installed');
    } finally {
      releaseRead.resolve();
      await tick;
    }
  });
}
