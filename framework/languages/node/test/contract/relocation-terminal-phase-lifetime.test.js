const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  encodeAuthorityAggregate
} = require('../../packages/framework/dist/runtime/locations/authority-relocation-record');

for (const outcome of ['success', 'failure']) {
  test(`terminal progress waits for admission and ${outcome} request delivery`, async () => {
    const fence = {
      nodeRid: 'target',
      nodeGeneration: 6n,
      ownerId: 'target-owner',
      ownerLeaseGeneration: 14n
    };
    const key = encodeAuthorityKey('actor', 'terminal');
    const events = [];
    let finish;
    const delivery = new Promise((resolve, reject) => {
      finish = outcome === 'success' ? resolve : reject;
    });
    let current = {
      kind: 'snapshot',
      ownerId: fence.ownerId,
      ownerLeaseGeneration: fence.ownerLeaseGeneration,
      storeVersion: { value: 'v1' },
      payload: Buffer.from('opaque'),
      relocation: {
        relocationId: '11111111-1111-1111-1111-111111111111',
        phase: 'prepared',
        source: { ...fence, nodeRid: 'source' },
        target: { ...fence, attemptGeneration: 1n },
        coordinator: { ...fence, expectedStoreVersion: 'source-v1' },
        sourceInventorySha256: '0'.repeat(64),
        applicationVersion: 1n,
        completion: {
          acceptedRequests: 1n,
          completedResults: 0n,
          pendingDeliveries: 0n,
          reference: null,
          sha256: null
        }
      },
      allocation: { objectKind: 'actor' }
    };
    const blobs = new Map();
    const runtimeOptions = {
      relocationStore: {
        async put(reference, bytes, retention) {
          blobs.set(reference.value, Buffer.from(bytes));
          return { kind: 'stored', storeNow: new Date(0), expiresAt: new Date(retention) };
        },
        async read(reference) {
          return blobs.has(reference.value)
            ? {
                kind: 'found',
                bytes: blobs.get(reference.value),
                storeNow: new Date(0),
                expiresAt: new Date(86400000)
              }
            : { kind: 'missing', storeNow: new Date(0) };
        }
      },
      locationStore: () => ({
        async readAuthority() {
          return current;
        },
        async compareExchangeAuthority(_key, expected, mutation) {
          assert.equal(expected.value, current.storeVersion.value);
          current = {
            ...current,
            relocation: mutation.relocation ?? undefined,
            storeVersion: { value: `${expected.value}:cas` }
          };
          const json = encodeAuthorityAggregate(undefined, current.relocation);
          events.push(json?.relocation?.phase ?? 'clear');
          assert.equal(json?.relocation?.aggregateGeneration, undefined);
          return { ...current, kind: 'stored' };
        }
      }),
      spotManager: () => undefined,
      actorTransfer: {}
    };
    const runtime = new ZLinkHostServiceRelocationRuntime(runtimeOptions);
    runtime.commitTargetReservation = async () => current;
    const stage = {
      phase: 'ready',
      lane: Promise.resolve(),
      cutoverReceived: true,
      boundaryRelay: [],
      offer: { prepareFingerprint: 'terminal', prepare: { target: fence } },
      staging: {
        primaryAuthorityKey: key,
        envelope: {
          aggregateId: current.relocation.relocationId,
          participants: [{ key: key.value }],
          memberships: []
        },
        hidden: new Map()
      },
      owner: {
        async publish() {
          events.push('publish');
        },
        async normalize() {
          await delivery;
          const {
            RelocationCompletionStore
          } = require('../../packages/framework/dist/runtime/locations/relocation-completion-store');
          const completion = new RelocationCompletionStore(
            runtimeOptions.locationStore(),
            runtimeOptions.relocationStore
          );
          const entry = {
            objectKey: key.value,
            source: {
              ownerId: 'requester',
              leaseGeneration: '1',
              nodeRid: '61',
              nodeGeneration: '1'
            },
            operationId: '00000000000000010000000000000001',
            replyRouteId: '1',
            reply: '',
            delivery: 'pending'
          };
          await completion.append(key, current, entry);
          await completion.acknowledge(key, current, entry, 'terminalReceived');
          events.push('delivered');
        },
        async openAdmission() {
          events.push('open');
        }
      },
      releaseActivation() {},
      releaseJoinAdmission() {}
    };
    const finalized = runtime.finalizeTargetStage('mesh', 'terminal', stage);
    const settled = Promise.allSettled([finalized]);
    await turn();
    assert.deepEqual(events, ['committed', 'activating', 'publish', 'activated', 'open']);
    finish(outcome === 'failure' ? new Error('delivery failed') : undefined);
    const [result] = await settled;
    if (outcome === 'success') {
      assert.equal(result.status, 'fulfilled');
      assert.deepEqual(events.slice(-4), ['delivered', 'cleaning', 'completed', 'clear']);
    } else {
      assert.equal(result.status, 'rejected');
      assert.equal(current.relocation.phase, 'activated');
      assert.equal(events.includes('clear'), false);
    }
  });
}

for (const phase of ['preparing', 'captured', 'prepared']) {
  test(`source cleanup records Aborted before removing ${phase} progress`, async () => {
    const fence = {
      nodeRid: 'source',
      nodeGeneration: 2n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 3n
    };
    const key = encodeAuthorityKey('actor', 'aborted');
    const phases = [];
    let current = {
      kind: 'snapshot',
      storeVersion: { value: 'prepared' },
      payload: Buffer.from('opaque'),
      relocation: {
        relocationId: '11111111-1111-1111-1111-111111111111',
        phase,
        source: fence,
        target:
          phase === 'prepared' ? { ...fence, nodeRid: 'target', attemptGeneration: 1n } : null,
        coordinator: { ...fence, expectedStoreVersion: 'original' },
        sourceInventorySha256: '0'.repeat(64),
        applicationVersion: 1n
      }
    };
    const original = current.relocation;
    const runtime = new ZLinkHostServiceRelocationRuntime({
      locationStore: () => ({
        async readAuthority() {
          return current;
        },
        async compareExchangeAuthority(_key, expected, mutation) {
          assert.equal(expected.value, current.storeVersion.value);
          assert.deepEqual(mutation.payload, Buffer.from('opaque'));
          if (mutation.relocation != null)
            assert.deepEqual(mutation.relocation, { ...original, phase: 'aborted' });
          current = {
            ...current,
            relocation: mutation.relocation ?? undefined,
            storeVersion: { value: `${expected.value}:cas` }
          };
          phases.push(encodeAuthorityAggregate(undefined, current.relocation)?.relocation?.phase);
          return { ...current, kind: 'stored' };
        }
      })
    });
    await runtime.releaseParticipantAuthorities(
      { participants: [{ key: key.value }], memberships: [] },
      '',
      undefined,
      'aborted'
    );
    assert.deepEqual(phases, ['aborted', undefined]);
  });
}

test('Actor destruction joins only the same incarnation target terminal', async () => {
  const runtime = new ZLinkHostServiceRelocationRuntime({});
  const actor = { context: { actorId: 'same-id' } };
  const otherIncarnation = { context: { actorId: 'same-id' } };
  let finish;
  const terminal = new Promise((resolve) => {
    finish = resolve;
  });
  runtime.targetStages.set('pending', {
    staging: { hidden: new Map([['actor', { actor }]]) },
    finalize: terminal
  });
  await runtime.drainTargetActorRelocation(otherIncarnation);
  let released = false;
  const destruction = runtime.drainTargetActorRelocation(actor).then(() => {
    released = true;
  });
  await turn();
  assert.equal(released, false, 'authority release must wait for this incarnation lifecycle');
  finish();
  await destruction;
  assert.equal(released, true);
});

for (const outcome of ['accepted', 'publication', 'deadline', 'unavailable']) {
  test(
    `cross-node Actor Join after activated relocation: ${outcome}`,
    { timeout: 10_000 },
    async () => {
      const {
        DefaultZLinkActorContext
      } = require('../../packages/framework/dist/runtime/actors/actor-context');
      const {
        ZLinkActorRuntimeState
      } = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
      const {
        runActorHandlerWithDeferredJoins
      } = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
      const { ZLinkFrameworkErrorKind, ZLinkSpotKind } = require('../../packages/framework/dist');
      const {
        ZLinkActorTransferRuntime
      } = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
      const actorId = 'returning-actor';
      const key = encodeAuthorityKey('actor', actorId);
      const relocationId = '11111111-1111-1111-1111-111111111111';
      const nodeGeneration = 6n;
      const ownerId = 'current-owner';
      const ownerLeaseGeneration = 14n;
      const fence = { nodeRid: '63757272656e74', nodeGeneration, ownerId, ownerLeaseGeneration };
      const next = { rid: 'next', lifecycleGeneration: 7n, applicationVersion: 1n };
      const completions = [];
      const targetActor = {
        context: { actorId },
        onJoinCompleted: (completion) => completions.push(completion)
      };
      const targetTransfer = new ZLinkActorTransferRuntime({ actorManager: () => undefined });
      let joinOperationId;
      let current = {
        kind: 'snapshot',
        ownerId,
        ownerLeaseGeneration,
        objectGeneration: 2n,
        authorityOwnerGeneration: 3n,
        storeVersion: { value: 'v1' },
        payload: Buffer.alloc(0),
        allocation: { objectKind: 'actor', stableType: 'player', descriptor: { rid: 'current' } },
        relocation: {
          relocationId,
          phase: outcome === 'unavailable' ? 'activated' : 'prepared',
          source: { ...fence, nodeRid: 'source' },
          target: { ...fence, attemptGeneration: 1n },
          coordinator: { ...fence, expectedStoreVersion: 'source-v1' },
          sourceInventorySha256: '0'.repeat(64),
          applicationVersion: 1n,
          completion: {
            acceptedRequests: 0n,
            completedResults: 0n,
            pendingDeliveries: 0n,
            reference: null,
            sha256: null
          }
        }
      };
      let releaseOpening;
      const openingHeld = new Promise((resolve) => {
        releaseOpening = resolve;
      });
      let openAdmission;
      const opened = new Promise((resolve) => {
        openAdmission = resolve;
      });
      let releasePublication;
      const publicationHeld = new Promise((resolve) => {
        releasePublication = resolve;
      });
      let publicationStarted;
      const clearing = new Promise((resolve) => {
        publicationStarted = resolve;
      });
      let joinRead;
      const joinObserved = new Promise((resolve) => {
        joinRead = resolve;
      });
      let joining = false;
      let captureCount = 0;
      const store = {
        async readAuthority() {
          if (joining) joinRead();
          return current;
        },
        async compareExchangeAuthority(_key, expected, mutation) {
          assert.equal(expected.value, current.storeVersion.value);
          if (mutation.relocation === null) {
            publicationStarted();
            await publicationHeld;
          }
          current = {
            ...current,
            payload: mutation.payload,
            relocation: mutation.relocation ?? undefined,
            storeVersion: { value: `${expected.value}:cas` }
          };
          return { ...current, kind: 'stored' };
        }
      };
      const runtime = new ZLinkHostServiceRelocationRuntime({
        registration: {
          spotNodes: new Map([
            [
              'play',
              { actorFactoryRegistrations: { player: { relocation: { kind: 'recreate' } } } }
            ]
          ])
        },
        meshNode: () => ({
          status: () => ({ routingId: 'current', lifecycleGeneration: nodeGeneration })
        }),
        liveDescriptors: async () => [next],
        locationStore: () => store,
        relocationStore: {},
        spotManager: () => undefined,
        actorTransfer: {
          async prepareMaintenanceSession() {
            captureCount++;
            return { handoffBacklog: [], async rollback() {} };
          }
        }
      });
      runtime.commitTargetReservation = async () => current;
      runtime.waitForRelocationBudgetHeadroom = async () => {};
      runtime.runCoordinator = async (_mesh, target) => {
        current = { ...current, allocation: { ...current.allocation, descriptor: target } };
        const actorRef = {
          actorId,
          objectGeneration: current.objectGeneration,
          meshName: 'play',
          nodeRid: target.rid
        };
        const completion = targetTransfer.prepareDeferredJoinAccepted(
          actorId,
          joinOperationId,
          actorRef,
          Buffer.alloc(0)
        );
        await targetTransfer.deliverDeferredJoinCompletion(
          completion,
          targetActor,
          actorRef,
          (operation) => operation()
        );
      };
      let terminal;
      const stage = {
        phase: 'ready',
        lane: Promise.resolve(),
        offer: { prepareFingerprint: 'previous', prepare: { target: fence } },
        staging: {
          primaryAuthorityKey: key,
          envelope: {
            aggregateId: relocationId,
            participants: [{ key: key.value }],
            memberships: []
          },
          hidden: new Map()
        },
        owner: {
          async publish() {},
          normalize: () => Promise.resolve(),
          async openAdmission() {
            openAdmission();
            if (outcome !== 'publication') await openingHeld;
          }
        },
        releaseActivation() {},
        releaseJoinAdmission() {}
      };
      if (outcome !== 'unavailable') {
        const wireHalf = BigInt('0x1111111111111111');
        const stagingId = `${wireHalf}:${wireHalf}:1`;
        runtime.targetStages.set(stagingId, stage);
        terminal = runtime.beginTargetFinalize('play', stagingId, stage);
      }
      try {
        if (terminal !== undefined) await Promise.race([opened, terminal]);
        assert.equal(current.relocation.phase, 'activated');
        if (outcome === 'publication') {
          // The Join starts while the completed relocation is still published.
          releaseOpening();
          await Promise.race([clearing, terminal]);
          assert.equal(current.relocation.phase, 'completed');
        }
        const state = new ZLinkActorRuntimeState(actorId);
        state.setNativeActorRef({ actorId, nodeRid: 'current', generation: 2n });
        const errors = [];
        const context = new DefaultZLinkActorContext(
          state,
          {
            messageFlow: () => ({
              begin: () => ({ trace: (event) => errors.push(event.errorMessage) })
            }),
            async joinSpot(
              actor,
              runtimeState,
              spotId,
              _request,
              _timeout,
              signal,
              operationId,
              deadlineAtMs
            ) {
              joinOperationId = operationId;
              const result = await runtime.relocateActorJoin({
                meshName: 'play',
                actor,
                state: runtimeState,
                target: {
                  spotId,
                  spotKind: ZLinkSpotKind.User,
                  targetNodeRid: next.rid,
                  targetNodeGeneration: next.lifecycleGeneration
                },
                relocationId: '22222222-2222-2222-2222-222222222222',
                signal,
                deadlineAtMs
              });
              return { accepted: true, actor: result.actorRef };
            }
          },
          undefined,
          undefined,
          () => 'play'
        );
        const actor = { context, onJoinCompleted: (completion) => completions.push(completion) };
        await state.getOrStartCreation('player', false, async () => {
          state.bindActor(actor, context);
          return { status: 'created', actor };
        }).task;
        joining = true;
        const join = runActorHandlerWithDeferredJoins(() => {
          context
            .joinSpot('room')
            .timeout(outcome === 'deadline' ? 250 : 5_000)
            .defer();
        });
        await Promise.race([joinObserved, join]);
        if (outcome === 'publication') {
          await turn();
          assert.deepEqual(
            completions.map(({ status }) => status),
            [],
            errors.join('\n')
          );
          assert.equal(captureCount, 0, 'capture must wait for publication release');
          releasePublication();
          await join;
          assert.equal(completions.length, 1);
          assert.equal(completions[0].status, 'accepted', errors.join('\n'));
          assert.equal(completions[0].actor.nodeRid, next.rid);
          assert.equal(captureCount, 1);
        } else if (outcome === 'accepted') {
          await turn();
          assert.deepEqual(
            completions.map(({ status }) => status),
            [],
            errors.join('\n')
          );
          assert.equal(captureCount, 0, 'capture must wait until the previous flow finishes');
          releaseOpening();
          await Promise.race([clearing, terminal]);
          assert.equal(current.relocation.phase, 'completed');
          await turn();
          assert.equal(captureCount, 0, 'terminal progress must include publication release');
          releasePublication();
          await join;
          assert.equal(completions.length, 1);
          assert.equal(completions[0].status, 'accepted');
          assert.equal(completions[0].actor.nodeRid, next.rid);
          assert.equal(current.allocation.descriptor.rid, next.rid);
          assert.equal(captureCount, 1);
        } else {
          await join;
          assert.equal(completions.length, 1);
          assert.equal(completions[0].status, 'failed');
          assert.equal(
            completions[0].kind,
            outcome === 'deadline'
              ? ZLinkFrameworkErrorKind.DeadlineExceeded
              : ZLinkFrameworkErrorKind.Unavailable,
            errors.join('\n')
          );
          assert.equal(captureCount, 0);
          assert.equal(current.relocation.phase, 'activated');
          if (outcome === 'unavailable') {
            await runActorHandlerWithDeferredJoins(() => context.joinSpot('room').defer());
            assert.equal(completions.length, 2);
            assert.equal(completions[1].status, 'failed');
            assert.equal(
              completions[1].kind,
              ZLinkFrameworkErrorKind.Unavailable,
              errors.join('\n')
            );
          }
        }
      } finally {
        releaseOpening();
        releasePublication();
        await terminal;
      }
    }
  );
}
