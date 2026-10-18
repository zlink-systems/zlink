const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  runActorHandlerWithDeferredJoins
} = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
const {
  resolveLifecycleHandler
} = require('../../packages/framework/dist/runtime/handlers/handler-instance-scope');
const runner = new framework.ZLinkRuntimeTaskRunner(
  new framework.ZLinkRuntimeTaskErrorSink(),
  new AbortController().signal
);

for (const notification of ['delayed', 'lost']) {
  test(`committed remote Join retires source registration before ${notification} leave and same-ID return`, async () => {
    const {
      ServiceStatefulRegistry
    } = require('../../packages/framework/dist/runtime/foundation/service-stateful-registry');
    const {
      ZLinkHostServiceRelocationRuntime
    } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
    const {
      encodeAuthorityKey
    } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
    const registry = new ServiceStatefulRegistry('source', 1n);
    const sourceSpot = registry.restoreSpot(
      { spotId: 'matchmaking', generation: 1n },
      'user',
      'Matchmaking',
      1n
    );
    const sourceRef = registry.createActor('returning-player', 'player', sourceSpot.ref).ref;
    const target = {
      routerChannelId: 'play',
      targetNodeRid: 'target',
      spotId: 'room',
      spotKind: framework.ZLinkSpotKind.User,
      targetSpotGeneration: 7n,
      targetNodeGeneration: 11n
    };
    const sourceAuthority = {
      kind: 'snapshot',
      objectGeneration: sourceRef.generation,
      authorityOwnerGeneration: 1n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      payload: Buffer.alloc(0),
      allocation: {
        state: 'active',
        objectKind: 'actor',
        stableType: 'player',
        descriptor: { meshName: 'play', rid: 'source' },
        descriptorLifecycleGeneration: 1n
      }
    };
    const committed = {
      ...sourceAuthority,
      authorityOwnerGeneration: 2n,
      ownerId: 'target-owner',
      allocation: {
        ...sourceAuthority.allocation,
        descriptor: { meshName: 'play', rid: 'target' },
        descriptorLifecycleGeneration: 11n
      }
    };
    let nativeDiscards = 0;
    const node = {
      status: () => ({ routingId: 'source', lifecycleGeneration: 1n }),
      createActor: () => sourceRef,
      actorLookup() {
        const current = registry.actor(sourceRef.actorId);
        return current === undefined
          ? undefined
          : {
              actor: current.ref,
              spotId: current.spot.spotId,
              spotGeneration: current.spot.generation,
              membershipEpoch: current.membershipEpoch
            };
      },
      entrySpot: () => ({ routingId: 'source' }),
      discardRelocatedActor(ref) {
        nativeDiscards++;
        registry.destroyActor(ref);
      },
      restoreActorAuthority(
        actorId,
        type,
        generation,
        owner,
        spotId,
        spotGeneration,
        membershipEpoch
      ) {
        return registry.restoreActor(
          { actorId, nodeRid: 'source', generation },
          type,
          { spotId, generation: spotGeneration },
          membershipEpoch,
          owner
        ).ref;
      },
      joinActorSpot() {
        return { high: 1n, low: 1n };
      },
      completionTable: {
        async submit(operation) {
          operation();
          return {
            terminalResult: 0,
            failureErrno: 0,
            operationKind: 7,
            parts: [],
            kindData: {
              kind: 'actorJoinCompletion',
              joinResult: 0,
              actor: { ...sourceRef, nodeRid: target.targetNodeRid },
              location: {
                actor: sourceRef,
                spotId: target.spotId,
                spotGeneration: 7n,
                membershipEpoch: 2n
              }
            }
          };
        }
      }
    };
    class PlayerFactory {
      create(context) {
        return { context };
      }
    }
    let source;
    let manager;
    let releaseLeave;
    let notifyLeave;
    const leaveStarted = new Promise((resolve) => {
      notifyLeave = resolve;
    });
    const leave = new Promise((resolve) => {
      releaseLeave = resolve;
    });
    const runtime = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([
          ['play', { actorFactoryRegistrations: { player: { relocation: { kind: 'recreate' } } } }]
        ])
      },
      meshNode: () => node,
      actorManager: () => manager,
      liveDescriptors: async () => [{ rid: 'target', lifecycleGeneration: 11n }],
      locationStore: () => ({ readAuthority: async () => sourceAuthority }),
      actorTransfer: {
        async prepareMaintenanceSession() {
          return { handoffBacklog: [], async commit() {} };
        },
        async completeRelocationSourceLeave(actor, spotId) {
          assert.equal(actor, source);
          assert.equal(spotId, sourceSpot.ref.spotId);
          notifyLeave();
          await leave;
        }
      }
    });
    // The transport/Store boundary confirms target-only CAS. Source admission,
    // capture, commit, caller metadata and both registry owners remain real.
    runtime.waitForRelocationBudgetHeadroom = async () => {};
    runtime.runCoordinator = async (_mesh, _descriptor, _authority, captured) => {
      await captured.commitSource(
        new Map([[encodeAuthorityKey('actor', sourceRef.actorId).value, committed]])
      );
    };
    let relocationId;
    manager = new framework.DefaultZLinkActorManager(
      {
        actorMeshNameProvider: () => 'play',
        actorFactories: new Map([['player', PlayerFactory]]),
        nativeActorNode: node,
        joinCoordinator: new framework.ZLinkActorNativeJoinCoordinator({
          node,
          completionTableProvider: () => node.completionTable,
          reportSourceLeaveError(error) {
            throw error;
          },
          spotRouteResolver: { resolve: async () => target },
          actorJoinRelocation: {
            relocateActorJoin(input) {
              relocationId = input.relocationId;
              return runtime.relocateActorJoin(input);
            }
          }
        })
      },
      runner
    );
    source = await manager.getOrCreateActor(sourceRef.actorId, 'player');
    const sourceState = manager.getState(sourceRef.actorId);
    sourceState.setJoinedSpot(sourceSpot.ref.spotId, undefined, 1n, 1n);
    let sourceDisposed;
    const disposed = new Promise((resolve) => {
      sourceDisposed = resolve;
    });
    class SourceHandler {
      dispose() {
        sourceDisposed();
      }
    }
    await resolveLifecycleHandler(source, SourceHandler, { create: (type) => new type() });
    await runActorHandlerWithDeferredJoins(() => source.context.joinSpot(target.spotId).defer());
    assert.equal(
      manager.getState(sourceRef.actorId),
      undefined,
      'committed source must leave the current instance registry'
    );
    assert.equal(
      registry.actor(sourceRef.actorId),
      undefined,
      'native source membership must retire at commit'
    );
    assert.equal(
      registry.closeSpot(sourceSpot.ref),
      true,
      'Shutdown must be able to close the source Spot without a leave delivery'
    );
    assert.equal(nativeDiscards, 1);

    const returnedSpot = registry.restoreSpot(
      { spotId: 'returned-room', generation: 7n },
      'user',
      'Room',
      3n
    );
    const returned = await manager.prepareRelocationActor(
      sourceRef.actorId,
      'player',
      sourceRef.generation,
      3n,
      returnedSpot.ref.spotId,
      7n,
      3n
    );
    manager.publishRelocationActor(sourceRef.actorId);
    assert.notEqual(returned, source);
    if (notification === 'delayed') {
      assert.equal(
        runtime.acceptActorJoinSourceLeave({
          transferId: relocationId,
          actorId: sourceRef.actorId,
          succeeded: true
        }),
        true
      );
    }
    const draining = runtime.drainActorJoinSources('play');
    await leaveStarted;
    assert.equal(await manager.findActor(sourceRef.actorId), returned);
    releaseLeave();
    await draining;
    await disposed;
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(
      await manager.findActor(sourceRef.actorId),
      returned,
      'late leave must dispose only its exact source scope'
    );
    assert.equal(registry.actor(sourceRef.actorId).spot.spotId, returnedSpot.ref.spotId);
    assert.equal(
      nativeDiscards,
      1,
      'late source cleanup must not discard the returned native instance'
    );
    assert.equal(
      runtime.acceptActorJoinSourceLeave({
        transferId: relocationId,
        actorId: sourceRef.actorId,
        succeeded: true
      }),
      true
    );
    await manager.releaseSpotActorMembers('play', returnedSpot.ref.spotId);
    assert.equal(registry.closeSpot(returnedSpot.ref), true);
  });
}
