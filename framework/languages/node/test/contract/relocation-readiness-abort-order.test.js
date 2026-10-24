const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
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
    locationStore: () => ({ readAuthority: async () => authority })
  });
  runtime.spotRegistration = () => ({ relocation: { kind: 'recreate' } });
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
