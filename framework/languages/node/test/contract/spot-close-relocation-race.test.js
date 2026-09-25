const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist/contracts');
const { ZLinkSpotSerialTurnExecutor } = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

for (const { casCommits, path } of [
  { casCommits: true, path: 'shell' },
  { casCommits: false, path: 'shell' },
  { casCommits: true, path: 'aggregate' }
]) {
  test(`Spot ${path} relocation waits for pending Close CAS before sealing (${casCommits ? 'stored' : 'failed'})`, async () => {
    let releaseCas;
    const casDecision = new Promise((resolve) => { releaseCas = resolve; });
    let enteredDecision;
    const decisionEntered = new Promise((resolve) => { enteredDecision = resolve; });
    let enteredSeal;
    const sealEntered = new Promise((resolve) => { enteredSeal = resolve; });
    let closing = false;
    const events = [];
    const activation = {
      spotId: 'spot-race',
      serial: new ZLinkSpotSerialTurnExecutor(),
      captureRelocation() {
        events.push('execution:seal');
        return Promise.resolve({ timers: [] });
      },
      abortRelocation() { events.push('execution:abort'); }
    };
    const runtime = new ZLinkHostServiceRelocationRuntime({
      locationStore: () => ({ readAuthority: async () => ({
        kind: 'snapshot',
        storeVersion: { value: 'ready-v1' },
        payload: Buffer.alloc(0),
        objectGeneration: 1n,
        authorityOwnerGeneration: 1n,
        ownerId: 'owner',
        ownerLeaseGeneration: 1n,
        allocation: { state: 'active', objectKind: 'user_spot', stableType: 'Spot' },
        storeNow: new Date()
      }) }),
      spotManager: () => ({
        pendingSpotCloseDecision() {
          enteredDecision();
          return casDecision;
        },
        isSpotClosing: () => closing
      }),
      meshNode: () => ({
        status: () => ({ routingId: 'source', lifecycleGeneration: 1n }),
        sealSpotMessageFollowIngress() {
          events.push('message-follow:seal');
          enteredSeal();
          return { spotId: 'spot-race' };
        },
        abortSpotMessageFollowIngress() {
          events.push('message-follow:abort');
          return true;
        },
        commitSpotMessageFollowIngress() {}
      })
    });
    runtime.spotRegistration = () => ({ relocation: { kind: 'allowed' } });
    runtime.waitForRelocationBudgetHeadroom = async () => {};
    runtime.runCoordinator = async (_mesh, _target, _authority, captured) => {
      await captured.abortSource();
    };

    const relocation = path === 'shell'
      ? runtime.relocatePerActorSpotShell('mesh', activation, { rid: 'target' }, undefined)
      : runtime.relocateSpotAggregate('mesh', activation, 'instance_spot', [], { rid: 'target' }, undefined);
    await Promise.race([decisionEntered, sealEntered]);
    assert.deepEqual(events, []);
    closing = casCommits;
    releaseCas();
    if (casCommits) {
      await assert.rejects(relocation, (error) => {
        assert.equal(error.kind, ZLinkFrameworkErrorKind.Unavailable);
        return true;
      });
      assert.deepEqual(events, []);
    } else {
      await relocation;
      assert.deepEqual(events, [
        'message-follow:seal', 'execution:seal',
        'execution:abort', 'message-follow:abort'
      ]);
    }
  });
}

test('Spot relocation seals synchronously when Close has no pending CAS', () => {
  const events = [];
  const runtime = new ZLinkHostServiceRelocationRuntime({
    spotManager: () => ({
      pendingSpotCloseDecision: () => undefined,
      isSpotClosing: () => false
    })
  });
  const result = runtime.afterSpotCloseDecision('mesh', 'spot-race', () => {
    events.push('sealed');
    return 7;
  });
  assert.equal(result, 7);
  assert.deepEqual(events, ['sealed']);
});
