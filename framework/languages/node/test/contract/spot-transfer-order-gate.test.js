const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');

const nodeRoot = path.resolve(__dirname, '../..');

for (const failedStage of ['handoff', 'leave', 'both']) {
  test(`source retirement maintenance commits once after ${failedStage} failure`, async () => {
    const failure = new Error(`failed ${failedStage}`);
    const events = [];
    const actor = { context: { actorId: 'maintenance-retirement' } };
    const state = {
      spotId: 'source',
      nativeActorRef: { actorId: actor.context.actorId, generation: 9n },
      beginMove() {},
      endMove() {
        events.push('ended');
      }
    };
    const runtime = new ZLinkActorTransferRuntime({
      actorHandoff: {
        isActive() {
          return false;
        },
        begin() {},
        snapshot() {
          return [];
        },
        complete() {
          events.push('handoff');
          if (failedStage !== 'leave') throw failure;
        }
      },
      spotManager: () => ({
        async beginActorTransfer() {},
        async commitActorLeaveAfterTransfer() {
          events.push('leave');
          if (failedStage !== 'handoff') throw failure;
        }
      })
    });
    const prepared = await runtime.prepareMaintenanceSession(actor, state);
    await assert.rejects(prepared.commit({}, {}, {}), (error) => {
      if (failedStage === 'both') {
        assert.ok(error instanceof AggregateError);
        assert.deepEqual(error.errors, [failure, failure]);
      } else assert.strictEqual(error, failure);
      return true;
    });
    await prepared.commit({}, {}, {});
    assert.deepEqual(events, ['handoff', 'leave', 'ended']);
  });
}

for (const failedStage of [
  undefined,
  'leave',
  'release',
  'retire',
  'core-leave',
  'all',
  'framework'
]) {
  test(`source retirement continues once after ${failedStage ?? 'successful'} cleanup`, async () => {
    const events = [];
    const failure = new Error(`failed ${failedStage}`);
    const diagnostics = [];
    let finishCleanup;
    const cleanup = new Promise((resolve) => {
      finishCleanup = resolve;
    });
    const actor = { context: { actorId: 'actor-source-cleanup' } };
    const state = {
      actorType: 'player',
      spotId: 'source-spot',
      nativeActorRef: {
        actorId: actor.context.actorId,
        nodeRid: 'source-node',
        generation: 9n
      },
      locationGeneration: 3n,
      ownerLeaseGeneration: 5n,
      ownsLocation: true,
      markLocationReleased() {
        events.push('location-released');
      },
      beginMove() {
        events.push('move-began');
      },
      endMove() {
        events.push('move-ended');
      }
    };
    const sourceAuthority = {
      kind: 'snapshot',
      storeVersion: { value: 'source-version' },
      payload: framework.encodeActorAuthorityIdentity({
        actorType: 'player',
        actor: {
          actorId: actor.context.actorId,
          objectGeneration: 9n,
          meshName: 'mesh',
          nodeRid: 'source-node'
        },
        meshName: 'mesh',
        ownerNodeGeneration: 3n,
        owner: { ownerId: 'source-owner', leaseGeneration: 5n },
        spotId: 'source-spot',
        spotGeneration: 1n,
        spotKind: framework.ZLinkSpotKind.User
      }),
      objectGeneration: 9n,
      authorityOwnerGeneration: 3n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 5n,
      allocation: {
        state: 'active',
        objectKind: 'actor',
        stableType: 'player',
        descriptor: { meshName: 'mesh', rid: 'source-node' },
        descriptorLifecycleGeneration: 3n,
        capacity: { actors: 1, spots: 0 }
      },
      storeNow: new Date(0)
    };
    const targetAuthority = {
      ...sourceAuthority,
      storeVersion: { value: 'target-version' },
      ownerId: 'target-owner',
      ownerLeaseGeneration: 13n,
      authorityOwnerGeneration: 11n,
      allocation: {
        ...sourceAuthority.allocation,
        descriptor: { meshName: 'mesh', rid: 'target-node' },
        descriptorLifecycleGeneration: 7n
      }
    };
    let targetCommitted = false;
    const authorityStore = {
      async readAuthority() {
        return targetCommitted ? targetAuthority : sourceAuthority;
      },
      async compareExchangeAuthority() {
        assert.fail('the source transfer must not mutate Actor authority');
      }
    };
    const runtime = new ZLinkActorTransferRuntime({
      routeTransport: {},
      spotManager: () => ({
        async beginActorTransfer() {
          events.push('source-sealed');
        },
        async prepareActorLeaveForTransfer() {
          events.push('source-leave-prepared');
        },
        async commitActorLeaveAfterTransfer() {
          events.push('source-membership-removed');
          if (failedStage === 'leave' || failedStage === 'all') throw failure;
        }
      }),
      actorManager: () => ({
        getState: () => state,
        async completeCoreRelocationSource() {
          events.push('registry-removed');
          if (failedStage === 'retire' || failedStage === 'all') throw failure;
        }
      }),
      primaryMeshNode: () => ({}),
      async notifyEntrySpotActorLeft() {},
      async restoreEntrySpotActorJoined() {},
      locationLifecycle: () => ({
        async releaseActor() {
          events.push('location-release');
          if (failedStage === 'release' || failedStage === 'all') throw failure;
        }
      }),
      actorHandoff: {
        begin() {
          events.push('handoff-began');
        },
        isActive() {
          return false;
        },
        snapshot() {
          return [];
        },
        snapshotCoreBacklog() {
          return [];
        },
        complete() {
          events.push('handoff-committed');
        }
      },
      actorTransferRegistry: {
        async transferOut() {
          return {
            state: framework.ZLinkMessage.fromEncoded(zlink.Message.from('state'))
          };
        }
      },
      authorityStore: () => authorityStore,
      relocationStore: () => undefined,
      clearRemoteActorPacketTarget() {},
      reportPostCommitError(error) {
        diagnostics.push(error);
      },
      onSourceDepartureCompleted(actorId) {
        events.push(`source-cleanup:${actorId}`);
        finishCleanup();
      }
    });

    const prepared = await runtime.prepareSource(
      actor,
      state,
      undefined,
      failedStage === 'framework' ? 'framework' : 'core'
    );
    if (failedStage !== 'framework') prepared.onSourceLeaveSubmitted(async () => {});
    const target = {
      routerChannelId: 'mesh',
      targetNodeRid: 'target-node',
      spotId: 'target-spot',
      spotKind: framework.ZLinkSpotKind.User,
      targetNodeGeneration: 7n,
      authorityOwnerGeneration: 11n,
      targetOwnerId: 'target-owner',
      ownerLeaseGeneration: 13n
    };
    const targetActorRef = {
      actorId: actor.context.actorId,
      nodeRid: 'target-node',
      objectGeneration: 9n,
      meshName: 'mesh'
    };
    targetCommitted = true;
    await prepared.observeTargetAuthority(target, targetActorRef);
    prepared.commit(target, targetActorRef, []);
    prepared.commit(target, targetActorRef, []);
    const leave =
      failedStage === 'framework'
        ? Promise.resolve()
        : runtime.notifyCoreSourceLeave(actor, async () => {
            if (failedStage === 'core-leave' || failedStage === 'all') throw failure;
          });
    if (failedStage === 'core-leave' || failedStage === 'all')
      await assert.rejects(leave, (error) => error === failure);
    else await leave;
    await cleanup;

    assert.ok(
      events.indexOf('source-membership-removed') <
        events.indexOf('source-cleanup:actor-source-cleanup')
    );
    assert.equal(events.at(-1), 'source-cleanup:actor-source-cleanup');
    assert.equal(events.filter((event) => event === 'source-membership-removed').length, 1);
    assert.equal(
      events.filter((event) => event === 'location-release').length,
      failedStage === 'framework' ? 0 : 1
    );
    assert.equal(events.filter((event) => event === 'registry-removed').length, 1);
    if (failedStage === 'all') {
      assert.equal(diagnostics.length, 1);
      assert.ok(diagnostics[0] instanceof AggregateError);
      assert.deepEqual(diagnostics[0].errors, [failure, failure, failure, failure]);
    } else {
      assert.deepEqual(
        diagnostics,
        failedStage === undefined || failedStage === 'framework' ? [] : [failure]
      );
    }
  });
}
