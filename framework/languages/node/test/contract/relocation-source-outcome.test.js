const { MessageChannel } = require('node:worker_threads');
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const {
  ServiceRelocationPostCommitError
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-coordinator');
const {
  ServiceRelocationObjectCaptureOwner,
  ServiceCapturedObjectRelocation
} = require('../../packages/framework/dist/runtime/foundation/service-relocation-object-owner');
const {
  ZLinkRuntimeMetrics
} = require('../../packages/framework/dist/runtime/diagnostics/runtime-metrics');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

for (const path of ['coordinator', 'shell', 'wide']) {
  for (const scenario of path === 'wide'
    ? ['preserve-cleanup', 'preserve-restore-failure', 'preserve-restore-failure-cleanup']
    : [
        'commit',
        'preserve',
        'preserve-cleanup',
        'preserve-restore-failure',
        'preserve-request-cleanup',
        'preserve-request-restore-failure',
        'preserve-stale-seal',
        'missing',
        'shutdown',
        'shutdown-during-session-wait',
        'cleanup-error',
        'cancel-after-commit',
        'cancel-without-preserve',
        ...(path === 'coordinator'
          ? ['window-preserve-cleanup', 'window-target-cleanup', 'window-store-primary']
          : [])
      ]) {
    test(`source relocation ${path} outcome: ${scenario}`, async () => {
      const samples = [];
      let sourceResumed = false;
      let failure;
      const cleanupFailure = new Error('source resume cleanup failed');
      const restoreFailure = new Error('source FIFO admission failed');
      const requestFailure = new Error('original request failure');
      const windowControl = scenario.startsWith('window-');
      const windowCleanupFailure = new Error('source window cleanup failed');
      const storeFailure = new TypeError('invalid Store authority');
      const windowCleanupObservations = [];
      let followExpired;
      const followExpiration = windowControl
        ? new Promise((resolve) => {
            followExpired = resolve;
          })
        : undefined;
      const instrument = (name) => ({
        add: (value, tags) => samples.push({ name, value, tags, sourceResumed }),
        record: (value, tags) => {
          samples.push({ name, value, tags });
          if (name === 'zlink.relocation.route_convergence') followExpired?.();
        },
        addCallback() {}
      });
      const metrics = new ZLinkRuntimeMetrics({
        getMeter: () => ({
          createCounter: instrument,
          createUpDownCounter: instrument,
          createHistogram: instrument,
          createObservableGauge: instrument
        })
      });
      const signal = new AbortController();
      const key = encodeAuthorityKey(
        path === 'coordinator' ? 'actor' : 'user_spot',
        'player'
      ).value;
      const primary = {
        kind: 'snapshot',
        storeVersion: { value: 'source-version' },
        objectGeneration: 1n,
        authorityOwnerGeneration: 1n,
        ownerId: 'source-owner',
        ownerLeaseGeneration: 1n,
        payload: Buffer.alloc(0),
        allocation: {
          state: 'active',
          objectKind: path === 'coordinator' ? 'actor' : 'user_spot',
          stableType: 'player',
          descriptor: { meshName: 'mesh', rid: 'target' },
          descriptorLifecycleGeneration: 2n
        }
      };
      const target = {
        rid: 'target',
        lifecycleGeneration: 2n,
        ownerId: 'target-owner',
        leaseGeneration: 2n,
        applicationVersion: 1n
      };
      const runtime = new ZLinkHostServiceRelocationRuntime({
        metrics,
        sourceQuiescenceChanged: () => {
          if (!windowControl || !runtime.isSafeToShutdown()) return;
          windowCleanupObservations.push({
            sourceResumed,
            completedOutcomes: samples
              .filter((sample) => sample.name === 'zlink.relocation.completed')
              .map((sample) => sample.tags.outcome)
          });
          throw windowCleanupFailure;
        },
        registration: { spotNodes: new Map([['mesh', {
          spotFactoryRegistrations: { player: { relocation: { kind: 'recreate' } } }
        }]]) },
        meshNode: () => ({
          status: () => ({ routingId: 'source', lifecycleGeneration: 1n }),
          sealSpotMessageFollowIngress: () => ({}),
          commitSpotMessageFollowIngress: () => true,
          abortSpotMessageFollowIngress: () => {
            if (scenario.startsWith('preserve') && scenario.includes('cleanup'))
              throw cleanupFailure;
            return true;
          }
        }),
        spotManager: () => ({
          isSpotClosing: () => false,
          pendingSpotCloseDecision: () => undefined,
          completeRelocationSource: async () => {}
        }),
        locationStore: () => ({ readAuthority: async () => primary })
      });
      runtime.sendControl = async (
        _mesh,
        _rid,
        prepare,
        requestSignal,
        _deadlineAtMs,
        onSubmitted
      ) => {
        if (scenario === 'shutdown-during-session-wait') {
          const pending = new Promise((_resolve, reject) => {
            runtime.pendingSessionRelocations.set('source-stop-probe', { reject });
          });
          return Promise.all([pending, runtime.dispose()]);
        }
        await onSubmitted(requestSignal);
        if (scenario.startsWith('preserve-request')) throw requestFailure;
        return { ...prepare, kind: 'ready', senderRole: 'target' };
      };
      runtime.submitControlFrame = async () => {
        if (scenario === 'shutdown') await runtime.dispose();
      };
      const settle = runtime.settleSourceAuthority.bind(runtime);
      runtime.settleSourceAuthority = async (...args) => {
        if (windowControl) await followExpiration;
        if (scenario === 'window-store-primary') throw storeFailure;
        if (scenario.startsWith('preserve') || scenario === 'window-preserve-cleanup')
          return 'source';
        if (scenario === 'missing') return 'lost';
        if (scenario === 'shutdown') {
          return settle(...args);
        }
        if (scenario === 'cancel-without-preserve') {
          signal.abort();
          throw new Error('caller cancelled');
        }
        return { kind: 'target', followAuthorities: new Map([[key, primary]]) };
      };
      const captured =
        path === 'coordinator'
          ? await new ServiceRelocationObjectCaptureOwner().captureStandaloneActor(
              '00000000-0000-0000-0000-000000000001',
              1n,
              {
                authorityKey: key,
                objectKind: 'actor',
                stableType: 'player',
                objectGeneration: 1n,
                authorityOwnerGeneration: 1n,
                seal: async () => ({
                  boundSessionState: Buffer.alloc(0),
                  queuedMessages: [],
                  timers: []
                }),
                captureApplicationState: async () => Buffer.alloc(0),
                commitSeal: async () => {
                  if (scenario === 'cancel-after-commit') signal.abort();
                  if (scenario === 'cleanup-error') throw new Error('source cleanup failed');
                },
                abortSeal: async (onCleanupError) => {
                  if (scenario.includes('restore-failure') || scenario === 'preserve-stale-seal')
                    throw restoreFailure;
                  sourceResumed = true;
                  if (scenario.startsWith('preserve') && scenario.includes('cleanup'))
                    onCleanupError(cleanupFailure);
                },
                discardSeal: async () => {}
              },
              {
                actorKey: key,
                spotKey: encodeAuthorityKey('user_spot', 'entry').value,
                spotObjectGeneration: 1n,
                membershipEpoch: 1n
              }
            )
          : undefined;
      const metric =
        path === 'coordinator' ? metrics.startRelocation('mesh', 'actor', 'recreate') : undefined;
      const keepAlive = windowControl ? new MessageChannel() : undefined;
      keepAlive?.port1.on('message', () => {});
      try {
        if (path === 'coordinator')
          await runtime.runCoordinator(
            'mesh',
            target,
            primary,
            captured,
            new Map([[key, primary]]),
            signal.signal,
            [],
            undefined,
            undefined,
            metric
          );
        else {
          const activation = {
            spotId: 'player',
            serial: new ZLinkSpotSerialTurnExecutor(),
            captureRelocation: async () => ({ timers: [] }),
            abortRelocation() {
              if (scenario.includes('restore-failure')) throw restoreFailure;
              if (scenario === 'preserve-stale-seal') return false;
              sourceResumed = true;
              return true;
            },
            commitRelocation: async () => {
              if (scenario === 'cancel-after-commit') signal.abort();
              if (scenario === 'cleanup-error') throw new Error('source cleanup failed');
              return true;
            }
          };
          if (path === 'wide')
            await runtime.relocateSpotAggregate(
              'mesh',
              activation,
              'user_spot',
              [],
              target,
              undefined,
              signal.signal
            );
          else
            await runtime.relocatePerActorSpotShell(
              'mesh',
              activation,
              target,
              undefined,
              signal.signal
            );
        }
      } catch (error) {
        failure = error;
        assert.ok(
          windowControl ||
            scenario.startsWith('preserve') ||
            [
              'preserve',
              'missing',
              'shutdown',
              'shutdown-during-session-wait',
              'cleanup-error',
              'cancel-without-preserve'
            ].includes(scenario),
          String(error)
        );
      } finally {
        keepAlive?.port1.close();
        keepAlive?.port2.close();
        await runtime.dispose();
      }
      const expected =
        scenario.startsWith('preserve') || scenario === 'window-preserve-cleanup'
          ? scenario.includes('restore-failure') || scenario === 'preserve-stale-seal'
            ? 'failed'
            : 'aborted'
          : scenario === 'shutdown' || scenario === 'shutdown-during-session-wait'
            ? 'shutdown'
            : ['missing', 'cancel-without-preserve', 'window-store-primary'].includes(scenario)
              ? 'failed'
              : 'completed';
      const completed = samples.filter((sample) => sample.name === 'zlink.relocation.completed');
      assert.equal(completed.length, 1);
      assert.equal(completed[0].tags.outcome, expected);
      if (expected === 'aborted') assert.equal(completed[0].sourceResumed, true);
      if (scenario.includes('restore-failure') || scenario === 'preserve-stale-seal') {
        assert.equal(sourceResumed, false);
        assert.equal(completed[0].sourceResumed, false);
      }
      if (windowControl) {
        if (scenario === 'window-target-cleanup') {
          assert(failure instanceof ServiceRelocationPostCommitError);
          assert.equal(failure.cause, windowCleanupFailure);
        } else
          assert.equal(
            failure,
            scenario === 'window-store-primary' ? storeFailure : windowCleanupFailure
          );
        assert.deepEqual(windowCleanupObservations, [
          {
            sourceResumed: scenario === 'window-preserve-cleanup',
            completedOutcomes: scenario === 'window-store-primary' ? [] : [expected]
          }
        ]);
        const convergence = samples.filter(
          (sample) => sample.name === 'zlink.relocation.route_convergence'
        );
        assert.equal(convergence.length, 1);
        assert.equal(runtime.isSafeToShutdown(), true);
      }
      if (scenario.startsWith('preserve-request')) assert.equal(failure, requestFailure);
      else if (scenario.startsWith('preserve') && scenario !== 'preserve') {
        assert(failure instanceof AggregateError);
        assert.equal(failure.errors.length, 1);
        if (path === 'wide') {
          assert(failure.errors[0] instanceof AggregateError);
          const originals = scenario.includes('restore-failure')
            ? scenario.includes('cleanup')
              ? [restoreFailure, cleanupFailure]
              : [restoreFailure]
            : [cleanupFailure];
          assert.equal(failure.errors[0].errors.length, originals.length);
          originals.forEach((error, index) => assert.equal(failure.errors[0].errors[index], error));
        } else if (scenario.includes('cleanup')) assert.equal(failure.errors[0], cleanupFailure);
        else if (scenario !== 'preserve-stale-seal' || path === 'coordinator')
          assert.equal(failure.errors[0], restoreFailure);
        else assert.match(failure.errors[0].message, /relocation seal became stale/);
      }
      assert.equal(
        samples.filter((sample) => sample.name === 'zlink.relocation.duration').length,
        1
      );
      assert.equal(
        samples.find((sample) => sample.name === 'zlink.relocation.duration').tags.outcome,
        expected
      );
      assert.equal(
        samples.filter((sample) => sample.name === 'zlink.relocation.started').length,
        1
      );
      assert.equal(samples.filter((sample) => sample.name === 'zlink.relocation.bytes').length, 1);
    });
  }
}

for (const actualFailures of [0, 1, 2]) {
  for (const receivesCleanup of [false, true]) {
    test(`capture rollback keeps error owners/actual=${actualFailures}/receiver=${receivesCleanup}`, async () => {
      const events = [];
      const first = new Error('first actual restore failed');
      const second = new Error('second actual restore failed');
      const cleanup = new Error('cleanup failed');
      const capture = new ServiceCapturedObjectRelocation({}, [
        {
          abortSeal() {
            events.push('first');
            if (actualFailures > 0) throw first;
          }
        },
        {
          abortSeal(onCleanupError) {
            events.push('second');
            onCleanupError(cleanup);
            if (actualFailures > 1) throw second;
          }
        }
      ]);
      const received = [];
      const onCleanup = receivesCleanup
        ? (error) => {
            assert.deepEqual(events, ['second', 'first']);
            received.push(error);
          }
        : undefined;
      if (actualFailures === 0 && receivesCleanup) {
        await capture.abortSource(onCleanup);
        assert.equal(received.length, 1);
        assert(received[0] instanceof AggregateError);
        assert.equal(received[0].errors.length, 1);
        assert.equal(received[0].errors[0], cleanup);
      } else {
        await assert.rejects(capture.abortSource(onCleanup), (error) => {
          assert(error instanceof AggregateError);
          const originals =
            actualFailures === 2
              ? [second, first, cleanup]
              : actualFailures === 1
                ? [first, cleanup]
                : [cleanup];
          assert.equal(error.errors.length, originals.length);
          originals.forEach((original, index) => assert.equal(error.errors[index], original));
          return true;
        });
        assert.equal(received.length, 0);
      }
      assert.deepEqual(events, ['second', 'first']);
    });
  }
}

for (const failure of ['membership', 'FIFO', 'both']) {
  test(`Actor rollback preserves actual failure and always opens replay/${failure}`, async () => {
    const events = [];
    const actual = new Error('FIFO admission failed');
    const membership = new Error('membership cancellation failed');
    const actor = { context: { actorId: 'source-rollback' } };
    const state = {
      spotId: 'source',
      nativeActorRef: { generation: 1n },
      isMoving: false,
      beginMove() {
        this.isMoving = true;
      },
      endMove() {
        this.isMoving = false;
        events.push('ended');
      }
    };
    const runtime = new ZLinkActorTransferRuntime({
      actorHandoff: {
        isActive: () => false,
        begin() {},
        snapshot: () => [],
        admitCanceledPrefix(_actorId, _queue, _preparation, start) {
          events.push('fifo-admit-attempt');
          void start.then(() => events.push('replay-open'));
          if (failure !== 'membership') throw actual;
          return { terminal: Promise.resolve() };
        }
      },
      spotManager: () => ({
        async beginActorTransfer() {},
        async cancelActorTransfer() {
          events.push('membership-cancel');
          if (failure !== 'FIFO') throw membership;
        }
      })
    });
    const prepared = await runtime.prepareMaintenanceSession(actor, state);
    await assert.rejects(
      prepared.rollback(),
      (error) => error === (failure === 'membership' ? membership : actual)
    );
    assert.equal(state.isMoving, false);
    assert.deepEqual(events, ['membership-cancel', 'fifo-admit-attempt', 'ended', 'replay-open']);
  });
}
