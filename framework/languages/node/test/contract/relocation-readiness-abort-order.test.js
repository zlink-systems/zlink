const { relocationAuthorityStore } = require('./fixtures/relocation-authority-store');
const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const {
  ZLinkSpotActivation
} = require('../../packages/framework/dist/runtime/spots/spot-activation-state');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  ZLinkUserSpotExecutionMode,
  ZLinkSpotRelocationCoordinationMode
} = require('../../packages/framework/dist/contracts');

test('precommit abort completes Continued before releasing held application work', async () => {
  const events = [];
  const serial = new ZLinkSpotSerialTurnExecutor(true);
  let callbackEntered;
  const entered = new Promise((resolve) => {
    callbackEntered = resolve;
  });
  let releaseCallback;
  const callbackTerminal = new Promise((resolve) => {
    releaseCallback = resolve;
  });
  const activation = new ZLinkSpotActivation({
    meshName: 'mesh',
    spotId: 'spot',
    domain: {
      kind: 'user',
      executionMode: ZLinkUserSpotExecutionMode.SpotWide,
      relocationCoordinationMode: ZLinkSpotRelocationCoordinationMode.ApplicationSignaled
    },
    spotType: class {},
    spot: {
      async onRelocationReadyCompleted() {
        events.push('continued');
        callbackEntered();
        await callbackTerminal;
        events.push('continued-completed');
      }
    },
    serial,
    timers: { captureRelocation: () => [], abortRelocation() {} },
    actorHandlers: {},
    handlers: {}
  });
  const authority = {
    kind: 'snapshot',
    storeVersion: { value: 'source-version' },
    objectGeneration: 1n,
    authorityOwnerGeneration: 1n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    payload: Buffer.alloc(0),
    allocation: {
      state: 'active',
      objectKind: 'user_spot',
      stableType: 'spot',
      descriptor: { meshName: 'mesh', rid: 'source' },
      descriptorLifecycleGeneration: 1n
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
    registration: {
      spotNodes: new Map([
        [
          'mesh',
          {
            spotFactoryRegistrations: { spot: { relocation: { kind: 'recreate' } } }
          }
        ]
      ])
    },
    meshNode: () => ({
      status: () => ({ routingId: 'source', lifecycleGeneration: 1n }),
      sealSpotMessageFollowIngress: () => ({}),
      abortSpotMessageFollowIngress: () => true,
      commitSpotMessageFollowIngress: () => true
    }),
    spotManager: () => ({
      isSpotClosing: () => false,
      pendingSpotCloseDecision: () => undefined
    }),
    locationStore: relocationAuthorityStore(async () => authority)
  });
  runtime.waitForRelocationBudgetHeadroom = async () => {};
  let boundaryRegistered;
  const registered = new Promise((resolve) => {
    boundaryRegistered = resolve;
  });
  const wait = activation.waitForRelocationBoundary.bind(activation);
  activation.waitForRelocationBoundary = (...args) => {
    const result = wait(...args);
    boundaryRegistered();
    return result;
  };
  let held;
  const failure = new Error('precommit abort');
  runtime.runCoordinator = async (_mesh, _target, _authority, captured) => {
    held = serial.post(() => {
      events.push('held');
    });
    await captured.abortSource();
    throw failure;
  };
  const relocating = runtime
    .relocateSpotAggregate('mesh', activation, 'user_spot', [], target)
    .catch((error) => error);
  await registered;
  await serial.execute(() => {
    events.push('turn');
    activation.relocationReadyCall().defer();
  });
  await entered;
  const sealedDuringCallback = activation.executionBarrier.isSealed;
  releaseCallback();
  assert.equal(await relocating, failure);
  await held;
  assert.equal(sealedDuringCallback, true);
  assert.deepEqual(events, ['turn', 'continued', 'continued-completed', 'held']);
});

for (const failedPhase of ['preparing', 'captured']) {
  test(`capture failure records Aborted with null target after ${failedPhase} before cleanup`, async () => {
    const {
      encodeAuthorityAggregate
    } = require('../../packages/framework/dist/runtime/locations/authority-relocation-record');
    const {
      encodeAuthorityKey
    } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
    const key = encodeAuthorityKey('instance_spot', 'abort-capture');
    const failure = new Error(`injected ${failedPhase} failure`);
    const events = [];
    let current = {
      kind: 'snapshot',
      storeVersion: { value: 'original' },
      objectGeneration: 1n,
      authorityOwnerGeneration: 1n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      payload: Buffer.from('opaque'),
      allocation: {
        state: 'active',
        objectKind: 'instance_spot',
        stableType: 'spot',
        descriptor: { meshName: 'mesh', rid: 'source' },
        descriptorLifecycleGeneration: 1n
      }
    };
    const activation = new ZLinkSpotActivation({
      meshName: 'mesh',
      spotId: 'abort-capture',
      domain: { kind: 'instance', objectGeneration: 1n },
      spotType: class {},
      spot: {},
      serial: new ZLinkSpotSerialTurnExecutor(true),
      timers: { captureRelocation: () => [], abortRelocation: () => events.push('rollback') },
      actorHandlers: {},
      handlers: {}
    });
    const runtime = new ZLinkHostServiceRelocationRuntime({
      registration: {
        spotNodes: new Map([
          [
            'mesh',
            {
              instanceSpotFactoryRegistrations: { spot: { relocation: { kind: 'recreate' } } }
            }
          ]
        ])
      },
      spotManager: () => ({
        isSpotClosing: () => false,
        pendingSpotCloseDecision: () => undefined
      }),
      meshNode: () => ({ status: () => ({ routingId: 'source', lifecycleGeneration: 1n }) }),
      locationStore: () => ({
        async readAuthority() {
          return current;
        },
        async compareExchangeAuthority(actual, expected, mutation) {
          assert.deepEqual(actual, key);
          assert.equal(expected.value, current.storeVersion.value);
          current = {
            ...current,
            relocation: mutation.relocation ?? undefined,
            storeVersion: { value: `${expected.value}:cas` }
          };
          events.push(
            encodeAuthorityAggregate(undefined, current.relocation)?.relocation?.phase ?? 'clear'
          );
          if (current.relocation?.phase === 'aborted') {
            assert.equal(current.relocation.target, null);
            assert.equal(current.relocation.coordinator.expectedStoreVersion, 'original');
          }
          if (current.relocation?.phase === 'captured') throw failure;
          return { ...current, kind: 'stored' };
        }
      })
    });
    runtime.waitForRelocationBudgetHeadroom = async () => {};
    runtime.captureApplication = async () => {
      if (failedPhase === 'preparing') throw failure;
      return Buffer.alloc(0);
    };
    await assert.rejects(
      runtime.relocateSpotAggregate('mesh', activation, 'instance_spot', [], {
        rid: 'target',
        applicationVersion: 1n
      }),
      (error) => error === failure
    );
    assert.deepEqual(
      events,
      failedPhase === 'preparing'
        ? ['preparing', 'aborted', 'clear', 'rollback']
        : ['preparing', 'captured', 'aborted', 'clear', 'rollback']
    );
    assert.equal(current.relocation, undefined);
  });
}
