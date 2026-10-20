const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const {
  ServiceRelocationObjectCaptureOwner
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

for (const path of ['coordinator', 'shell']) {
  for (const scenario of [
    'commit',
    'preserve',
    'missing',
    'shutdown',
    'shutdown-during-session-wait',
    'cleanup-error',
    'cancel-after-commit',
    'cancel-without-preserve'
  ]) {
    test(`source relocation ${path} outcome: ${scenario}`, async () => {
      const samples = [];
      let sourceResumed = false;
      const instrument = (name) => ({
        add: (value, tags) => samples.push({ name, value, tags, sourceResumed }),
        record: (value, tags) => samples.push({ name, value, tags }),
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
      const key = encodeAuthorityKey(path === 'shell' ? 'user_spot' : 'actor', 'player').value;
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
          objectKind: path === 'shell' ? 'user_spot' : 'actor',
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
        meshNode: () => ({
          status: () => ({ routingId: 'source', lifecycleGeneration: 1n }),
          sealSpotMessageFollowIngress: () => ({}),
          commitSpotMessageFollowIngress: () => true,
          abortSpotMessageFollowIngress: () => true
        }),
        spotManager: () => ({
          isSpotClosing: () => false,
          pendingSpotCloseDecision: () => undefined,
          completeRelocationSource: async () => {}
        }),
        locationStore: () => ({ readAuthority: async () => primary })
      });
      runtime.sendControl = async (_mesh, _rid, prepare) => {
        if (scenario === 'shutdown-during-session-wait') {
          const pending = new Promise((_resolve, reject) => {
            runtime.pendingSessionRelocations.set('source-stop-probe', { reject });
          });
          return Promise.all([pending, runtime.dispose()]);
        }
        return { ...prepare, kind: 'ready', senderRole: 'target' };
      };
      runtime.submitControlFrame = async () => {
        if (scenario === 'shutdown') runtime.disposed = true;
      };
      runtime.spotRegistration = () => ({ relocation: { kind: 'recreate' } });
      const settle = runtime.settleSourceAuthority.bind(runtime);
      runtime.settleSourceAuthority = async (...args) => {
        if (scenario === 'preserve') return 'source';
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
                abortSeal: async () => {
                  sourceResumed = true;
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
        else
          await runtime.relocatePerActorSpotShell(
            'mesh',
            {
              spotId: 'player',
              serial: new ZLinkSpotSerialTurnExecutor(),
              captureRelocation: async () => ({ timers: [] }),
              abortRelocation() {
                sourceResumed = true;
              },
              commitRelocation: async () => {
                if (scenario === 'cancel-after-commit') signal.abort();
                if (scenario === 'cleanup-error') throw new Error('source cleanup failed');
                return true;
              }
            },
            target,
            undefined,
            signal.signal
          );
      } catch (error) {
        assert.ok(
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
        for (const window of runtime.sourceCutoverWindows.values())
          clearTimeout(window.followTimer);
      }
      const expected =
        scenario === 'preserve'
          ? 'aborted'
          : scenario === 'shutdown' || scenario === 'shutdown-during-session-wait'
            ? 'shutdown'
            : ['missing', 'cancel-without-preserve'].includes(scenario)
              ? 'failed'
              : 'completed';
      const completed = samples.filter((sample) => sample.name === 'zlink.relocation.completed');
      assert.equal(completed.length, 1);
      assert.equal(completed[0].tags.outcome, expected);
      if (expected === 'aborted') assert.equal(completed[0].sourceResumed, true);
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
