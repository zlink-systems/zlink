const { replyFixture } = require('./helpers/relocation-reply');
const { relocationInventoryStore } = require('./fixtures/relocation-authority-store');
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
const { ZLinkSpotKind } = require('../../packages/framework/dist');
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
    'a Join waits for publication while a captured request is still running'
  ],
  ['inside request', 'a captured request can defer a cross-node Join of its Actor while publication is held'],
  [
    'send before request',
    'a replayed send can defer a cross-node Join with a captured request still queued'
  ],
  [
    'same-node',
    'a same-node Join from a replayed send commits after the previous target finalization'
  ]
]) {
  test(name, { timeout: 10_000 }, async () => {
    const sameNode = scenario === 'same-node';
    const hasRequest = scenario !== 'send' && !sameNode;
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
        applicationVersion: 1n
      }
    };
    const replies = replyFixture({ ownerId: fence.ownerId, leaseGeneration: fence.ownerLeaseGeneration });
    await replies.setWriter(replies.writer);
    const publication = deferred();
    const store = {
      relocationReplies: replies.store,
      ...relocationInventoryStore(),
      async readAuthority() {
        return current;
      },
      async compareExchangeAuthority(_key, expected, mutation) {
        assert.equal(current.storeVersion.value, expected.value);
        // Replay must not wait for publication release: hold the release until the replayed send runs.
        if (scenario === 'send' && mutation.relocation === null) await sendStarted.promise;
        if (hasRequest && mutation.relocation === null) await publication.promise;
        // Keep finalization in flight while the replayed send starts its same-node Join.
        if (sameNode && mutation.relocation?.phase === 'cleaning') {
          await sendStarted.promise;
          await turn();
        }
        // A writer that changed the row meanwhile makes this exchange a conflict.
        assert.equal(current.storeVersion.value, expected.value, 'authority row conflict');
        current = {
          ...current,
          payload: mutation.payload,
          relocation: mutation.local ? current.relocation : (mutation.relocation ?? undefined),
          storeVersion: { value: `${expected.value}:cas` }
        };
        events.push(mutation.local ? 'local-commit' : (current.relocation?.phase ?? 'release'));
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
          joinRead.resolve();
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

    if (hasRequest) {
      const packet = packets.find((candidate) => candidate.returnResponse);
      await replies.seed({
        relocationId,
        objectKey: key.value,
        operationId: packet.messageFollowContext.operationId,
        replyRouteId: packet.source.replyRouteId,
        source: {
          ownerId: packet.source.ownerId,
          leaseGeneration: packet.source.ownerLeaseGeneration,
          nodeRid: Buffer.from(packet.source.nodeRid).toString('hex'),
          nodeGeneration: packet.source.nodeGeneration
        }
      });
    }

    const requestStarted = deferred();
    const finishRequest = deferred();
    const replySent = deferred();
    const acknowledgeReply = deferred();
    const sendStarted = deferred();
    const joinRead = deferred();
    let joinTask;
    const startJoin = () =>
      runActorHandlerWithDeferredJoins(() => {
        context.joinSpot('room').timeout(1_500).defer();
      });

    // The membership commit of a same-node Join: wait for the previous target flow, then
    // read and exchange the authority row (actor-location-claims commitMembership).
    const localJoinErrors = [];
    const localJoinInActorTurn = async () => {
      try {
        await runtime.awaitPreviousActorRelocation(actorId);
        const read = await store.readAuthority();
        await store.compareExchangeAuthority(key, read.storeVersion, {
          kind: 'put',
          local: true,
          payload: read.payload
        });
      } catch (error) {
        localJoinErrors.push(String(error));
      } finally {
        joinFinished();
      }
    };

    const joinInActorTurn = async () => {
      await startJoin();
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
                    if (sameNode) await localJoinInActorTurn();
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

    const hidden = { actor, replayPackets: packets };
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
      if (hasRequest) {
        if (scenario === 'outside request') {
          await requestStarted.promise;
          joinTask = startJoin();
        }
        await joinRead.promise;
        await turn();
        assert.equal(completions.length, 0, errors.filter(Boolean).join('\n'));
        assert.equal((await replies.rows(relocationId))[0].state, 'accepted');
        assert.ok(!events.includes('capture'));
        if (sendBeforeRequest) assert.ok(!events.includes('request'));
        publication.resolve();

        await (joinTask ?? joined);
        assert.deepEqual(
          completions.map(({ status, kind }) => ({ status, kind })),
          [{ status: 'accepted', kind: undefined }],
          errors.filter(Boolean).join('\n')
        );
        assert.ok(events.indexOf('open') < events.indexOf('release'));
        assert.ok(events.indexOf('release') < events.indexOf('capture'));
        assert.ok(!events.includes('reply'));
        if (sendBeforeRequest) finishReplay();
        finishRequest.resolve();
        await replySent.promise;
        assert.equal((await replies.rows(relocationId))[0].state, 'pending');
        acknowledgeReply.resolve();
        finishReplay();
      } else if (sameNode) {
        await Promise.race([joined, finalization.then(() => joined)]);
        assert.deepEqual(localJoinErrors, []);
        assert.ok(events.indexOf('open') < events.indexOf('handler'));
        assert.ok(events.indexOf('handler') < events.indexOf('cleaning'));
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
            'local-commit'
          ]
        );
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
      await stage.terminal;
      assert.equal(runtime.targetStages.has(stagingId), false);
      if (!hasRequest) {
        assert.ok(!events.includes('replay-terminal'));
      }
    } finally {
      resumeReplay();
      publication.resolve();
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
      assert.ok(events.indexOf('release') < events.indexOf('ack'));
    } else {
      assert.equal(events.at(-1), 'replay-terminal');
    }
  });
}
