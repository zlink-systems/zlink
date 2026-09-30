const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkActorPacketRelay
} = require('../../packages/framework/dist/runtime/host/actor-packet-relay');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');

function rid(value) {
  return zlink.RoutingId.from(value);
}

// Session–Actor binding §5·§6·§8: a Session and its Actor on the same node form one current
// binding like a remote one, so an Actor relocation seals that binding (command 42) and carries
// it to the target. TicTacToe lost this binding and failed its post-Join push under load.
test('a same-node Session binding is installed once and carried by the relocation seal', async () => {
  const actorId = 'actor-same-node';
  const localNode = rid('play-a');
  const actorRef = { nodeRid: localNode, actorId, generation: 9n };
  const state = new framework.ZLinkActorRuntimeState(actorId);
  state.setNativeActorRef(actorRef);
  state.setLocationGeneration(3n);
  state.setOwnerLeaseGeneration(5n);

  const relay = new ZLinkActorPacketRelay({
    routeTransport: {},
    streamBindingRuntime: () => ({ find() {} }),
    meshRouters: {
      remoteBoundSessionTargetForSource(sourceNodeRid) {
        return {
          routerChannelId: 'play.route',
          targetNodeRid: sourceNodeRid,
          spotId: sourceNodeRid
        };
      }
    },
    actorManager: () => ({ getState: (requested) => (requested === actorId ? state : undefined) }),
    spotManager: () => undefined,
    spotNodeRuntime: () => ({ primaryMeshNode: { status: () => ({ routingId: localNode }) } }),
    errorSink: () => ({ reportRuntimeTaskException() {} })
  });

  // The Session owner's registry snapshot of the same-node bind.
  await relay.confirmRemoteSessionBinding(
    { actorId, objectGeneration: 9n, meshName: 'play', nodeRid: localNode },
    localNode,
    rid('session-7'),
    undefined,
    {
      binding: {
        sessionRid: 'session-7',
        actor: actorRef,
        sessionOwnerNodeRid: 'play-a',
        sessionOwnerNodeGeneration: 2n,
        sessionOwnerId: 'play-a',
        sessionOwnerLeaseGeneration: 2n,
        bindingGeneration: 4n,
        membershipEpoch: 1n
      }
    }
  );
  assert.equal(state.boundSessionBindingGeneration, 4n);
  assert.equal(String(state.boundSession.sessionRid), 'session-7');
  // The local Session registry serves a same-node binding; remote routing sees no target.
  assert.equal(state.remoteBoundSessionTarget, undefined);

  const relocation = { high: 7n, low: 9n };
  const seals = [];
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {},
    spotManager: () => undefined,
    actorManager: () => ({ getState: () => state }),
    primaryMeshNode: () => ({
      status: () => ({ routingId: localNode, lifecycleGeneration: 2n })
    }),
    async notifyEntrySpotActorLeft() {},
    async restoreEntrySpotActorJoined() {},
    locationLifecycle: () => undefined,
    actorHandoff: {
      begin() {},
      sealConnectionBoundIngress() {},
      snapshot() {
        return [];
      },
      failPending() {}
    },
    actorTransferRegistry: {},
    authorityStore: () => ({
      async readAuthority() {
        return {
          kind: 'snapshot',
          storeVersion: { value: 'authority-1' },
          payload: Buffer.alloc(0),
          objectGeneration: 9n,
          authorityOwnerGeneration: 3n,
          ownerId: 'play-a-owner',
          ownerLeaseGeneration: 5n,
          allocation: {
            state: 'active',
            objectKind: 'actor',
            stableType: 'player',
            descriptor: { meshName: 'play', rid: localNode },
            descriptorLifecycleGeneration: 2n,
            capacity: { actors: 1, spots: 0 }
          },
          storeNow: new Date(0)
        };
      }
    }),
    relocationStore: () => undefined,
    liveDescriptors: async () => [
      { rid: localNode, lifecycleGeneration: 2n, ownerId: 'play-a-owner', leaseGeneration: 5n }
    ],
    sessionRelocationWire: () => ({
      async requestSessionRelocationSeal(meshName, targetNodeRid, request) {
        seals.push({ meshName, targetNodeRid, request });
        return {
          relocation: request.relocation,
          coordinator: request.coordinator,
          actor: request.actor,
          session: request.session
        };
      },
      async sendSessionRelocationRoute() {}
    }),
    clearRemoteActorPacketTarget() {}
  });

  const prepared = await runtime.prepareMaintenanceSession(
    { context: { actorId } },
    state,
    undefined,
    false,
    relocation
  );

  assert.equal(seals.length, 1);
  assert.equal(String(seals[0].targetNodeRid), 'play-a');
  assert.equal(seals[0].request.session.sessionRid, 'session-7');
  assert.equal(seals[0].request.session.bindingGeneration, 4n);
  assert.notEqual(prepared.target, undefined, 'the relocation carries the same-node binding');
  assert.equal(String(prepared.target.sessionRid), 'session-7');
  assert.deepEqual(prepared.target.serviceWireRelocation.relocation, relocation);
  // The seal belongs to the relocation operation; the Actor keeps its one binding.
  assert.equal(state.boundSession.serviceWireRelocation, undefined);
  prepared.discard(new Error('test end'));
  state.endMove();
});
