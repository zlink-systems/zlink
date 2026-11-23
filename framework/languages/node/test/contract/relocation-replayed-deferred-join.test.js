const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: turn } = require('node:timers/promises');
const { Message } = require('@zlink-systems/zlink');
const {
  encodeStreamHeader,
  ZLinkStreamMessageKind,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags
} = require('../../packages/framework/dist/runtime/streams/protocol');
const { ZLinkSpotKind, ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const {
  ZLinkActorHandoffCoordinator,
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/internal');
const {
  DefaultZLinkActorContext
} = require('../../packages/framework/dist/runtime/actors/actor-context');
const {
  runZLinkActorExecution
} = require('../../packages/framework/dist/runtime/actors/actor-execution-context');
const {
  runActorHandlerWithDeferredJoins
} = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

for (const [scenario, name] of [
  ['send', 'a replayed send runs with dispatch opening and its Join waits for publication release'],
  [
    'outside request',
    'a Join waits for captured request delivery while a replayed send is still running'
  ],
  ['inside request', 'a Join deferred by the captured request fails immediately with Unavailable'],
  [
    'send before request',
    'a Join deferred by a replayed send fails immediately while the same Actor has a captured request queued'
  ]
]) {
  test(name, { timeout: 10_000 }, async () => {
    const hasRequest = scenario !== 'send';
    const sendBeforeRequest = scenario === 'send before request';
    const actorId = 'returning-actor';
    const key = encodeAuthorityKey('actor', actorId);
    const fence = {
      nodeRid: '63757272656e74',
      nodeGeneration: 6n,
      ownerId: 'current-owner',
      ownerLeaseGeneration: 14n
    };
    const next = {
      rid: 'next',
      lifecycleGeneration: 7n,
      applicationVersion: 1n
    };
    const relocationId = '11111111-1111-1111-1111-111111111111';
    const half = BigInt('0x1111111111111111');
    const stagingId = `${half}:${half}:1`;
    const events = [];
    const errors = [];
    const completions = [];
    let current = {
      kind: 'snapshot',
      ownerId: fence.ownerId,
      ownerLeaseGeneration: fence.ownerLeaseGeneration,
      objectGeneration: 2n,
      authorityOwnerGeneration: 3n,
      storeVersion: { value: 'v1' },
      payload: Buffer.alloc(0),
      allocation: {
        objectKind: 'actor',
        stableType: 'player',
        descriptor: { rid: 'current' }
      },
      relocation: {
        relocationId,
        phase: 'committed',
        source: { ...fence, nodeRid: '736f75726365' },
        target: { ...fence, attemptGeneration: 1n },
        coordinator: { ...fence, expectedStoreVersion: 'source-v1' },
        sourceInventorySha256: '0'.repeat(64),
        applicationVersion: 1n,
        completion: {
          acceptedRequests: hasRequest ? 1n : 0n,
          completedResults: 0n,
          pendingDeliveries: 0n,
          reference: null,
          sha256: null
        }
      }
    };
    const store = {
      async readAuthority() {
        if (outsideJoin) joinRead.resolve();
        return current;
      },
      async compareExchangeAuthority(_key, expected, mutation) {
        assert.equal(current.storeVersion.value, expected.value);
        // Replay must not wait for publication release: hold the release until the replayed send runs.
        if (scenario === 'send' && mutation.relocation === null) await sendStarted.promise;
        current = {
          ...current,
          payload: mutation.payload,
          relocation: mutation.relocation ?? undefined,
          storeVersion: { value: `${expected.value}:cas` }
        };
        events.push(current.relocation?.phase ?? 'release');
        return { ...current, kind: 'stored' };
      }
    };

    const state = new ZLinkActorRuntimeState(actorId);
    state.setNativeActorRef({ actorId, nodeRid: 'current', generation: 2n });
    let joinOperationId;
    let runtime;
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

    const actor = {
      context,
      onJoinCompleted: (completion) => completions.push(completion)
    };
    await state.getOrStartCreation('player', false, async () => {
      state.bindActor(actor, context);
      return { status: 'created', actor };
    }).task;

    const replay = new ZLinkActorHandoffCoordinator({
      messageFollowDurationMs: 30,
      isStaleActorRef: () => false,
      requestSource: () => ({
        meshName: 'play',
        objectGeneration: 2n,
        ownerId: fence.ownerId,
        ownerLeaseGeneration: fence.ownerLeaseGeneration,
        nodeRid: 'current',
        nodeRidHex: fence.nodeRid,
        nodeGeneration: fence.nodeGeneration,
        authorityOwnerGeneration: 3n
      }),
      currentOwnerFence: () => ({
        ...fence,
        nodeRid: 'current',
        nodeRidHex: fence.nodeRid,
        ownerLeaseGeneration: fence.ownerLeaseGeneration
      })
    });
    replay.beginProvisional(actorId, 2n);
    const parts = [Message.from('header'), Message.from('RoomAssignment')];
    const requestParts = [
      Message.from(
        encodeStreamHeader({
          kind: ZLinkStreamMessageKind.Request,
          codec: ZLinkStreamCodec.Json,
          flags: ZLinkStreamHeaderFlags.None,
          requestSeq: 1n,
          name: 'Request',
          metadata: new Map()
        })
      ),
      Message.from('Request')
    ];
    let packets;
    try {
      const actorRef = {
        nodeRid: 'current',
        actorId,
        objectGeneration: 2n,
        meshName: 'play'
      };
      const captureSend = () =>
        replay.capture('remote', actorId, parts, false, undefined, actorRef);
      if (sendBeforeRequest) await captureSend();
      if (hasRequest) {
        const response = replay.capture(
          'remote',
          actorId,
          requestParts,
          true,
          undefined,
          actorRef,
          undefined,
          {
            sourceNodeRid: 'requester',
            originalOperation: { high: 1n, low: 2n },
            originalReplyRouteId: 1n,
            requestSource: {
              ownerId: 'requester-owner',
              leaseGeneration: 1n,
              nodeRid: 'requester',
              nodeGeneration: 1n
            }
          }
        );
        void response.catch(() => undefined);
      }
      if (!sendBeforeRequest) await captureSend();
      packets = replay.snapshot(actorId);
    } finally {
      replay.cancel(actorId);
      [...parts, ...requestParts].forEach((part) => part.close());
    }
    assert.equal(packets.length, hasRequest ? 2 : 1);
    if (hasRequest) assert.ok(packets[sendBeforeRequest ? 1 : 0].source);

    const requestStarted = deferred();
    const finishRequest = deferred();
    const replySent = deferred();
    const acknowledgeReply = deferred();
    const sendStarted = deferred();
    const joinRead = deferred();
    let outsideJoin = false;
    let elapsed;
    let joinTask;
    const startJoin = () =>
      runActorHandlerWithDeferredJoins(() => {
        context.joinSpot('room').timeout(1_500).defer();
      });

    const joinInActorTurn = async () => {
      const started = performance.now();
      await startJoin();
      elapsed = performance.now() - started;
      joinFinished();
    };

    let resumeReplay;
    const replayHeld = new Promise((done) => {
      resumeReplay = done;
    });
    let joinFinished;
    const joined = new Promise((done) => {
      joinFinished = done;
    });
    let finishReplay;
    const replayTerminalHeld = new Promise((done) => {
      finishReplay = done;
    });

    const transfer = new ZLinkActorTransferRuntime({
      actorHandoff: replay,
      actorManager: () => undefined,
      spotManager: () => ({
        admitSourceActorPacketPrefix(_actorId, records) {
          return {
            terminal: (async () => {
              for (const record of records) {
                record.onWithdrawable(() => false);
                await record.preparation.prepare(new AbortController().signal);
                await replayHeld;
                await record.drain((frames) =>
                  runZLinkActorExecution(actorId, undefined, async () => {
                    if (frames[1].data().toString() === 'Request') {
                      events.push('request');
                      requestStarted.resolve();
                      if (scenario === 'inside request') await joinInActorTurn();
                      await finishRequest.promise;
                      return { accepted: true };
                    }
                    assert.equal(frames[1].data().toString(), 'RoomAssignment');
                    events.push('handler');
                    sendStarted.resolve();
                    if (scenario === 'send' || sendBeforeRequest) await joinInActorTurn();
                    await replayTerminalHeld;
                    events.push('replay-terminal');
                  })
                );
              }
            })()
          };
        }
      }),
      reportPostCommitError(error) {
        errors.push(String(error));
      }
    });

    const blobs = new Map();
    runtime = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([
          [
            'play',
            {
              actorFactoryRegistrations: {
                player: { relocation: { kind: 'recreate' } }
              }
            }
          ]
        ])
      },
      meshNode: () => ({
        status: () => ({
          routingId: 'current',
          lifecycleGeneration: fence.nodeGeneration
        })
      }),
      actorManager: () => undefined,
      currentOwner: () => ({
        ownerId: fence.ownerId,
        leaseGeneration: fence.ownerLeaseGeneration
      }),
      liveDescriptors: async () => [next],
      locationStore: () => store,
      relocationStore: {
        async put(reference, bytes, retention) {
          blobs.set(reference.value, Buffer.from(bytes));
          return { kind: 'stored', storeNow: new Date(0), expiresAt: new Date(retention) };
        },
        async read(reference) {
          const bytes = blobs.get(reference.value);
          return bytes === undefined
            ? { kind: 'missing', storeNow: new Date(0) }
            : { kind: 'found', bytes, storeNow: new Date(0), expiresAt: new Date(86400000) };
        }
      },
      spotManager: () => undefined,
      actorTransfer: {
        async publishRoutedActorOwnership() {},
        async prepareMaintenanceSession() {
          events.push('capture');
          return { handoffBacklog: [], async rollback() {} };
        }
      }
    });
    runtime.sendReplyRelay = async () => {
      events.push('reply');
      replySent.resolve();
      await acknowledgeReply.promise;
      events.push('ack');
      return 'terminalReceived';
    };
    runtime.commitTargetReservation = async () => {
      events.push(current.relocation.phase);
      return current;
    };
    runtime.waitForRelocationBudgetHeadroom = async () => {};
    runtime.runCoordinator = async (_mesh, target) => {
      current = {
        ...current,
        allocation: { ...current.allocation, descriptor: target }
      };
      const actorRef = {
        actorId,
        objectGeneration: current.objectGeneration,
        meshName: 'play',
        nodeRid: target.rid
      };
      const completion = transfer.prepareDeferredJoinAccepted(
        actorId,
        joinOperationId,
        actorRef,
        Buffer.alloc(0)
      );
      await transfer.deliverDeferredJoinCompletion(completion, actor, actorRef, (operation) =>
        operation()
      );
    };

    const hidden = { actor };
    let opened;
    const admissionOpened = new Promise((done) => {
      opened = done;
    });
    let replayCompletion;
    const stage = {
      phase: 'ready',
      lane: Promise.resolve(),
      offer: {
        prepareFingerprint: 'previous',
        prepare: { target: { ...fence, nodeRid: 'current' }, targetAttemptGeneration: 1n }
      },
      staging: {
        primaryAuthorityKey: key,
        envelope: {
          aggregateId: relocationId,
          participants: [{ key: key.value, boundSessionState: Buffer.alloc(0) }],
          memberships: []
        },
        hidden: new Map([[key.value, hidden]])
      },
      owner: {
        async publish() {},
        normalize() {
          replayCompletion = transfer.admitRelocationPrefix(
            actor,
            2n,
            packets,
            hidden.replayStart,
            hidden.onReplayResult
          );
          return replayCompletion;
        },
        async openAdmission() {
          events.push('open');
          opened();
        }
      },
      releaseActivation() {},
      releaseJoinAdmission() {}
    };
    runtime.targetStages.set(stagingId, stage);
    const finalization = runtime.beginTargetFinalize('play', stagingId, stage);
    try {
      await Promise.race([admissionOpened, finalization]);
      resumeReplay();
      if (scenario === 'outside request') {
        await requestStarted.promise;
        outsideJoin = true;
        joinTask = startJoin();
        await joinRead.promise;
        await turn();
        assert.equal(current.relocation.phase, 'activated');
        assert.equal(current.relocation.completion.completedResults, 0n);
        assert.ok(!events.includes('capture'));

        finishRequest.resolve();
        await Promise.race([replySent.promise, finalization, joinTask]);
        assert.ok(events.includes('reply'), errors.filter(Boolean).join('\n'));
        await sendStarted.promise;
        assert.equal(current.relocation.completion.completedResults, 1n);
        assert.equal(current.relocation.completion.pendingDeliveries, 1n);
        assert.ok(!events.includes('capture'));

        acknowledgeReply.resolve();
        await joinTask;
        assert.equal(completions[0]?.status, 'accepted', errors.filter(Boolean).join('\n'));
        assert.ok(events.indexOf('ack') < events.indexOf('release'));
        assert.ok(events.indexOf('release') < events.indexOf('capture'));
      } else if (scenario === 'inside request' || sendBeforeRequest) {
        await Promise.race([joined, finalization]);
        assert.deepEqual(
          completions.map(({ status, kind }) => ({ status, kind })),
          [{ status: 'failed', kind: ZLinkFrameworkErrorKind.Unavailable }],
          errors.filter(Boolean).join('\n')
        );
        assert.ok(elapsed < 500, `Join took ${elapsed}ms with a 1500ms deadline`);
        assert.equal(current.relocation.phase, 'activated');
        assert.ok(!events.includes('capture'));
        if (sendBeforeRequest) {
          assert.ok(!events.includes('request'));
          assert.equal(current.relocation.completion.completedResults, 0n);
          finishReplay();
          await Promise.race([requestStarted.promise, finalization]);
        }
        finishRequest.resolve();
        acknowledgeReply.resolve();
        await Promise.race([sendStarted.promise, finalization]);
      } else {
        await Promise.race([joined, finalization.then(() => joined)]);
        assert.deepEqual(
          completions.map(({ status, kind }) => ({ status, kind })),
          [{ status: 'accepted', kind: undefined }],
          errors.filter(Boolean).join('\n')
        );
        assert.ok(events.indexOf('open') < events.indexOf('handler'));
        assert.ok(events.indexOf('handler') < events.indexOf('release'));
        assert.ok(events.indexOf('release') < events.indexOf('capture'));
        assert.deepEqual(
          events.filter((event) => event !== 'handler'),
          [
            'committed',
            'activating',
            'activated',
            'open',
            'cleaning',
            'completed',
            'release',
            'capture',
            'preparing',
            'captured'
          ]
        );
      }
      if (scenario === 'inside request') finishReplay();
      await stage.terminal;
      assert.equal(runtime.targetStages.has(stagingId), false);
      if (scenario !== 'inside request' && !sendBeforeRequest) {
        assert.ok(!events.includes('replay-terminal'));
      }
    } finally {
      resumeReplay();
      finishRequest.resolve();
      acknowledgeReply.resolve();
      finishReplay();
      await joinTask;
      await finalization.catch((error) => {
        throw error.cause ?? error;
      });
      await replayCompletion;
    }
    if (sendBeforeRequest) {
      assert.ok(events.indexOf('replay-terminal') < events.indexOf('request'));
      assert.ok(events.indexOf('ack') < events.indexOf('release'));
    } else if (scenario !== 'inside request') {
      assert.equal(events.at(-1), 'replay-terminal');
    }
  });
}
