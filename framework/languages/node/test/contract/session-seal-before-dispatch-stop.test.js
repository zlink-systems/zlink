const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

// Relocation 04 §4.1 (#1877): when an Actor is bound to a Session, the Session
// owner seals that binding (command 42/43) before the source stops the Actor's
// application dispatch. Session messages that arrive after the seal wait at the
// Session owner; they never enter the source handoff backlog.

function rid(value) {
  return zlink.RoutingId.from(value);
}

function boundSource(actorId) {
  const sourceNode = rid('source-node');
  const sessionNode = rid('session-node');
  const state = new framework.ZLinkActorRuntimeState(actorId);
  state.setNativeActorRef({ nodeRid: sourceNode, actorId, generation: 9n });
  state.setLocationGeneration(3n);
  state.setOwnerLeaseGeneration(5n);
  state.installBoundSessionBinding({
    routerChannelId: 'play',
    targetNodeRid: sessionNode,
    spotId: sessionNode,
    sessionNodeRid: sessionNode,
    sessionRid: rid('session-7'),
    sessionOwnerNodeGeneration: 2n,
    sessionOwnerId: 'session-owner',
    sessionOwnerLeaseGeneration: 4n,
    bindingGeneration: 6n
  });
  return { actor: { context: { actorId } }, state, sourceNode, sessionNode };
}

function sourceRuntime(source, events, options = {}) {
  const handoff = { begun: false };
  const runtime = new ZLinkActorTransferRuntime({
    routeTransport: {},
    spotManager: () =>
      options.beginActorTransfer === undefined
        ? undefined
        : {
            beginActorTransfer: options.beginActorTransfer,
            async cancelActorTransfer() {}
          },
    actorManager: () => ({ getState: () => source.state }),
    primaryMeshNode: () => ({
      status: () => ({ routingId: source.sourceNode, lifecycleGeneration: 2n })
    }),
    async notifyEntrySpotActorLeft() {},
    locationLifecycle: () => undefined,
    actorHandoff: {
      isActive: () => handoff.begun,
      isProvisional: () => false,
      begin() {
        handoff.begun = true;
        events.push('source:handoff-begin');
      },
      cancel() {
        handoff.begun = false;
      },
      admitCanceledPrefix() {
        handoff.begun = false;
        return { terminal: Promise.resolve() };
      },
      sealConnectionBoundIngress() {
        events.push('source:connection-bound-sealed');
      },
      snapshot: () => [],
      failPending() {}
    },
    authorityStore: () => ({
      async readAuthority() {
        return {
          kind: 'snapshot',
          storeVersion: { value: 'authority-1' },
          payload: Buffer.alloc(0),
          objectGeneration: 9n,
          authorityOwnerGeneration: 3n,
          ownerId: 'source-owner',
          ownerLeaseGeneration: 5n,
          allocation: {
            state: 'active',
            objectKind: 'actor',
            stableType: 'player',
            descriptor: { meshName: 'play', rid: source.sourceNode },
            descriptorLifecycleGeneration: 2n,
            capacity: { actors: 1, spots: 0 }
          },
          storeNow: new Date(0)
        };
      }
    }),
    relocationStore: () => undefined,
    liveDescriptors: async () => [
      {
        rid: source.sessionNode,
        lifecycleGeneration: 2n,
        ownerId: 'session-owner',
        leaseGeneration: 4n
      }
    ],
    sessionRelocationWire: () => ({
      async requestSessionRelocationSeal(_meshName, _targetNodeRid, request) {
        // The Session owner observes the source at the moment it installs the seal.
        events.push(
          `session:seal moving=${source.state.isMoving} handoff=${handoff.begun ? 'begun' : 'idle'}`
        );
        if (options.sealFailure !== undefined) throw options.sealFailure;
        return {
          relocation: request.relocation,
          coordinator: request.coordinator,
          actor: request.actor,
          session: request.session
        };
      },
      async sendSessionRelocationRoute(_meshName, _targetNodeRid, route) {
        events.push(`session:route ${route.route.action}`);
      }
    }),
    clearRemoteActorPacketTarget() {}
  });
  return { runtime, handoff };
}

const relocation = { high: 7n, low: 9n };

for (const manageMembership of [true, false]) {
  test(`maintenance relocation seals the Session before the source stops dispatch (manageMembership=${manageMembership})`, async () => {
    const events = [];
    const source = boundSource(`seal-order-maintenance-${manageMembership}`);
    const { runtime } = sourceRuntime(source, events);

    const prepared = await runtime.prepareMaintenanceSession(
      source.actor,
      source.state,
      undefined,
      manageMembership,
      relocation
    );
    try {
      assert.deepEqual(events, [
        'session:seal moving=false handoff=idle',
        'source:handoff-begin',
        'source:connection-bound-sealed'
      ]);
      assert.notEqual(prepared.target, undefined);
    } finally {
      prepared.discard(new Error('test end'));
      source.state.endMove();
    }
  });
}

test('maintenance rollback after the dispatch stop restores the source and releases the seal', async () => {
  const events = [];
  const source = boundSource('seal-order-rollback');
  const { runtime, handoff } = sourceRuntime(source, events);

  const prepared = await runtime.prepareMaintenanceSession(
    source.actor,
    source.state,
    undefined,
    true,
    relocation
  );
  await prepared.rollback();
  assert.deepEqual(events, [
    'session:seal moving=false handoff=idle',
    'source:handoff-begin',
    'source:connection-bound-sealed',
    'session:route abort'
  ]);
  assert.equal(source.state.isMoving, false);
  assert.equal(handoff.begun, false);
});

test('a failed Session seal leaves the source dispatching', async () => {
  const events = [];
  const source = boundSource('seal-order-seal-failure');
  const failure = new Error('Session owner rejected command 42');
  const { runtime, handoff } = sourceRuntime(source, events, { sealFailure: failure });

  await assert.rejects(
    runtime.prepareMaintenanceSession(source.actor, source.state, undefined, true, relocation),
    (error) => error === failure
  );
  assert.deepEqual(events, ['session:seal moving=false handoff=idle']);
  assert.equal(source.state.isMoving, false);
  assert.equal(handoff.begun, false);
});

test('a dispatch stop that fails after the seal releases the matching seal', async () => {
  const events = [];
  const source = boundSource('seal-order-stop-failure');
  // A joined Spot makes the dispatch stop wait for the Spot's current turn.
  source.state.setJoinedSpot(rid('spot-1'));
  const failure = new Error('spot transfer refused');
  const { runtime, handoff } = sourceRuntime(source, events, {
    async beginActorTransfer() {
      events.push('source:spot-transfer');
      throw failure;
    }
  });

  await assert.rejects(
    runtime.prepareMaintenanceSession(source.actor, source.state, undefined, true, relocation),
    (error) => error === failure
  );
  assert.deepEqual(events, [
    'session:seal moving=false handoff=idle',
    'source:handoff-begin',
    'source:spot-transfer',
    'session:route abort'
  ]);
  assert.equal(source.state.isMoving, false);
  assert.equal(handoff.begun, false);
});

test('Spot relocation seals every member Session before the Spot capture stops dispatch', async () => {
  const events = [];
  const members = ['member-a', 'member-b'];
  const activation = {
    spotId: 'spot',
    serial: new ZLinkSpotSerialTurnExecutor(),
    captureRelocation() {
      events.push('spot:capture');
      return Promise.resolve({ timers: [] });
    },
    abortRelocation() {
      events.push('spot:abort');
    }
  };
  const stopped = new Error('test stops after the seal');
  const runtime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => ({
      readAuthority: async () => ({
        kind: 'snapshot',
        storeVersion: { value: 'v1' },
        payload: Buffer.alloc(0),
        objectGeneration: 5n,
        authorityOwnerGeneration: 11n,
        ownerId: 'owner',
        ownerLeaseGeneration: 13n,
        allocation: { state: 'active', objectKind: 'user_spot', stableType: 'Spot' },
        storeNow: new Date()
      })
    }),
    spotManager: () => ({
      pendingSpotCloseDecision: () => undefined,
      isSpotClosing: () => false
    }),
    meshNode: () => ({
      status: () => ({ routingId: 'source', lifecycleGeneration: 2n }),
      sealSpotMessageFollowIngress() {
        events.push('spot:follow-seal');
        return { spotId: 'spot' };
      },
      abortSpotMessageFollowIngress() {
        events.push('spot:follow-abort');
        return true;
      },
      commitSpotMessageFollowIngress() {}
    }),
    actorTransfer: {
      async prepareMaintenanceSession(actor) {
        events.push(`${actor.context.actorId}:session-sealed`);
        return {
          async rollback() {
            events.push(`${actor.context.actorId}:rollback`);
          }
        };
      }
    }
  });
  runtime.spotRegistration = () => ({ relocation: { kind: 'allowed' } });
  runtime.actorRegistration = () => ({ relocation: { kind: 'allowed' } });
  runtime.waitForRelocationBudgetHeadroom = async () => {};
  runtime.captureApplication = async () => {
    throw stopped;
  };
  runtime.runCoordinator = async () => {
    events.push('target:prepare');
  };
  const states = members.map((actorId) => ({
    actorId,
    actor: { context: { actorId } },
    actorType: 'Actor',
    spotMembershipEpoch: 1n
  }));
  try {
    await assert.rejects(
      runtime.relocateSpotAggregate(
        'mesh',
        activation,
        'user_spot',
        states,
        { rid: 'target' },
        undefined,
        undefined
      ),
      (error) => error === stopped
    );
    const capture = events.indexOf('spot:capture');
    const followSeal = events.indexOf('spot:follow-seal');
    assert.notEqual(capture, -1, events.join(', '));
    for (const actorId of members) {
      const sealed = events.indexOf(`${actorId}:session-sealed`);
      assert.notEqual(sealed, -1, events.join(', '));
      assert.ok(sealed < capture, `${actorId} seal precedes the Spot capture: ${events}`);
      assert.ok(sealed < followSeal, `${actorId} seal precedes the follow seal: ${events}`);
    }
  } finally {
    await runtime.dispose();
  }
});
