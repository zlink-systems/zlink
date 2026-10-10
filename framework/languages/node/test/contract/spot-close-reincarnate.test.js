'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkInMemoryAuthorityStore } = require('../../packages/framework/dist/runtime/locations/in-memory-authority-store');
const { ZLinkSpotLocationClaims } = require('../../packages/framework/dist/runtime/locations/spot-location-claims');
const { encodeAuthorityKey } = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { encodeServiceInstanceAuthorityPayload, decodeServiceInstanceAuthorityPayload } = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');
const protocol = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const fs = require('node:fs');
const path = require('node:path');
const closeFixture = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../../../../runtime/conformance/spot-close-v1.json'), 'utf8'));
const { ZLinkRuntimeTaskErrorSink, ZLinkRuntimeTaskRunner } = require('../../packages/framework/dist/runtime/execution');
const detachedTaskRunner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeTaskErrorSink(), new AbortController().signal);

function branchExpectation(name) {
  const branch = closeFixture.closeBranches?.find((candidate) => candidate.name === name);
  assert.ok(branch, `shared Close fixture branch '${name}' is required`);
  return branch.expect;
}

function deferred() {
  let resolve;
  const promise = new Promise((done) => { resolve = done; });
  return { promise, resolve };
}

async function authorityFixture(productionStore) {
  let live = true;
  const store = productionStore ?? new ZLinkInMemoryAuthorityStore({ isOwnerLive: () => live, isTargetLive: () => live });
  const key = encodeAuthorityKey('instance_spot', 'close-room');
  const target = { meshName: 'mesh', nodeRid: 'node', nodeLifecycleGeneration: 1n,
    owner: { ownerId: 'owner', leaseGeneration: 1n } };
  const payload = (state) => encodeServiceInstanceAuthorityPayload({ state, stableType: 'room',
    spotId: 'close-room', ownerId: 'owner', ownerLeaseGeneration: 1n,
    ownerMeshName: 'mesh', ownerNodeRid: 'node', ownerNodeGeneration: 1n });
  const reserved = await store.reserve({ key: { kind: 'instance_spot', globalId: 'close-room' },
    intent: { stableType: 'room', requestContentReference: 'close-node:test',
      requestSha256: Buffer.alloc(32, 1), requestEncodedSize: 1n }, target,
    creatingPayload: payload('coldActivating'), capacity: { actors: 0, spots: 1 } });
  assert.equal(reserved.kind, 'reserved');
  return { store, key, target, payload, reserved, expire: () => { live = false; } };
}

async function commitFixture(fixture) {
  const committed = await fixture.store.commit({ key: { kind: 'instance_spot', globalId: 'close-room' },
    reservationId: fixture.reserved.reservationId,
    expectedStoreVersion: fixture.reserved.creating.storeVersion.value,
    target: fixture.target, readyPayload: fixture.payload('ready') });
  assert.equal(committed.kind, 'committed');
  return committed.ready;
}

test('Reincarnate CAS rejects Reserved and expired lease and preserves owner and capacity', async () => {
  const fixture = await authorityFixture();
  const mutation = { kind: 'put', generationTransition: 'reincarnate', payload: fixture.payload('coldActivating') };
  assert.equal((await fixture.store.compareExchangeAuthority(fixture.key, fixture.reserved.creating.storeVersion, mutation)).kind, 'conflict');
  const before = await commitFixture(fixture);
  const closing = await fixture.store.compareExchangeAuthority(fixture.key, before.storeVersion,
    { kind: 'put', generationTransition: 'preserve', payload: fixture.payload('closing') });
  const after = await fixture.store.compareExchangeAuthority(fixture.key, closing.storeVersion, mutation);
  assert.equal(after.kind, 'stored');
  assert.notEqual(after.objectGeneration, before.objectGeneration);
  assert.notEqual(after.authorityOwnerGeneration, before.authorityOwnerGeneration);
  assert.equal(after.ownerId, before.ownerId);
  assert.equal(after.ownerLeaseGeneration, before.ownerLeaseGeneration);
  assert.deepEqual(after.allocation, before.allocation);
  assert.equal((await fixture.store.compareExchangeAuthority(fixture.key, closing.storeVersion, mutation)).kind, 'conflict');
  fixture.expire();
  const expired = await fixture.store.compareExchangeAuthority(fixture.key, after.storeVersion, mutation);
  assert.equal(expired.kind, 'conflict');
  assert.equal(expired.current.storeVersion.value, after.storeVersion.value);
});

test('opaque provider Reincarnate batches both generation counters and fences rejected writes', async () => {
  const now = new Date(100);
  const inner = new framework.ZLinkInMemoryProviderLocationStore(() => now);
  const batches = [];
  let rejectReincarnate = false;
  let ownerToken;
  const provider = {
    read: inner.read.bind(inner), scan: inner.scan.bind(inner),
    async write(request, signal) {
      const counters = request.mutations.filter((mutation) => mutation.key.value.endsWith('counter'));
      if (counters.length === 2) {
        batches.push(request);
        if (rejectReincarnate) {
          await store.releaseOwnerLease(ownerToken);
          return { kind: 'conflict', storeNow: now };
        }
      }
      return await inner.write(request, signal);
    }
  };
  const store = new framework.ZLinkLocationStoreRepository(provider, () => now);
  const lease = await store.claimOwnerLease('owner', 30_000);
  assert.equal(lease.kind, 'claimed');
  ownerToken = lease.token;
  await store.updateMeshNode({ meshName: 'mesh', rid: 'node', lifecycleGeneration: 1n,
    descriptorRevision: 1n, endpoint: 'tcp://node', objectRole: framework.ZLinkObjectRole.Server,
    placementWeight: 1, populationCapacity: { actors: { active: 0, reserved: 0, limit: 1 },
      spots: { active: 0, reserved: 0, limit: 1 }, spotTypes: [] },
    activationConcurrency: { active: 0, limit: 1 }, channelWeights: {}, applicationVersion: 1n,
    spotTypes: [], objectCapabilities: [], state: framework.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'test', ownerId: 'owner', leaseGeneration: lease.token.leaseGeneration, updatedAt: now
  }, framework.ZLinkLocationWriteIntent.NewClaim);
  const fixture = await authorityFixture(store);
  const before = await commitFixture(fixture);
  const mutation = { kind: 'put', generationTransition: 'reincarnate', payload: fixture.payload('coldActivating') };
  const after = await store.compareExchangeAuthority(fixture.key, before.storeVersion, mutation);
  assert.equal(after.kind, 'stored');
  assert.notEqual(after.objectGeneration, before.objectGeneration);
  assert.notEqual(after.authorityOwnerGeneration, before.authorityOwnerGeneration);
  assert.deepEqual(after.allocation, before.allocation);
  const batch = batches.at(-1);
  assert.equal(batch.mutations.length, 3);
  for (const counter of batch.mutations.filter((entry) => entry.key.value.endsWith('counter'))) {
    assert.equal(batch.conditions.some((condition) => condition.key.value === counter.key.value), true);
  }
  rejectReincarnate = true;
  assert.equal((await store.compareExchangeAuthority(fixture.key, after.storeVersion, mutation)).kind, 'conflict');
  assert.equal((await store.readAuthority(fixture.key)).storeVersion.value, after.storeVersion.value);
});

test('Instance resolver delivers Closing to its exact current owner', async () => {
  const fixture = await authorityFixture();
  const ready = await commitFixture(fixture);
  const closing = await fixture.store.compareExchangeAuthority(fixture.key, ready.storeVersion,
    { kind: 'put', generationTransition: 'preserve', payload: fixture.payload('closing') });
  const { ZLinkInstanceActivationAuthority } = require('../../packages/framework/dist/runtime/host/instance-activation-authority');
  const resolver = new ZLinkInstanceActivationAuthority({ store: fixture.store, meshName: 'mesh', owner: () => fixture.target.owner });
  const result = await resolver.read({ targetSpotId: 'close-room', stableType: 'room', targetNodeRid: 'node', targetNodeGeneration: 1n, descriptorVersion: '1' });
  assert.equal(result.kind, 'ready');
  assert.equal(result.route.ownerId, closing.ownerId);
  assert.equal(result.route.objectGeneration, closing.objectGeneration);
  assert.equal(result.route.authorityOwnerGeneration, closing.authorityOwnerGeneration);
  assert.equal(result.route.storeVersion, closing.storeVersion.value);
  assert.equal((await resolver.read({ targetSpotId: 'close-room', stableType: 'room', targetNodeRid: 'node', targetNodeGeneration: 2n })).kind, 'missing');
});

test('Foundation preserves Missing ingress during Active cold initialization without publishing Ready', async () => {
  const fixture = await authorityFixture();
  const ready = await commitFixture(fixture);
  const cold = await fixture.store.compareExchangeAuthority(fixture.key, ready.storeVersion,
    { kind: 'put', generationTransition: 'reincarnate', payload: fixture.payload('coldActivating') });
  const { ZLinkInstanceActivationAuthority } = require('../../packages/framework/dist/runtime/host/instance-activation-authority');
  const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
  const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
  const { ApplicationIngressRecordOwner } = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
  const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 1n));
  const admitted = deferred();
  let ingress;
  const owner = new ServiceStatefulRuntime({
    setServiceIngress(handler) { ingress = handler; },
    mailbox: { tryEnqueue(record) { admitted.resolve(record); return true; } }
  }, 'node', 1n);
  owner.registerAsyncInstanceActivationAuthority(new ZLinkInstanceActivationAuthority({
    store: fixture.store, meshName: 'mesh', owner: () => fixture.target.owner
  }));
  owner.registerInstanceApplicationLifecycle({ isMaterialized: () => true, isMaterializing: () => true,
    materialize() { assert.fail('Closing owner must not materialize a Ready projection'); } });
  const target = { targetSpotId: 'close-room', stableType: 'room', targetNodeRid: 'node', targetNodeGeneration: 1n, descriptorVersion: '1' };
  const operation = { high: 1n, low: 9n };
  const deadline = BigInt(Date.now() + 10_000);
  const applicationJobOwner = ApplicationIngressRecordOwner.create(queue, await queue.acquire(undefined, 'remote'), { close() {} });
  ingress({ parts: [wire.encodeInstanceSpotActivationHeader(target, 1n, 'node', undefined,
    'send', operation, deadline), Buffer.from('payload')], sourceRoutingId: 'node', applicationJobOwner });
  const record = await admitted.promise;
  assert.equal(record.stateful.activationRecord.activation, 'missing');
  assert.deepEqual(record.stateful.activationRecord.operation, operation);
  assert.equal(record.stateful.activationRecord.deadlineUnixMs, deadline);
  assert.equal(record.stateful.targetSpot.generation, cold.objectGeneration);
  assert.equal((await fixture.store.readAuthority(fixture.key)).storeVersion.value, cold.storeVersion.value);
  record.applicationJob.close();
  applicationJobOwner.close();
});

for (const { state, sealed } of [
  { state: framework.ZLinkFrameworkRuntimeState.Draining, sealed: false },
  { state: framework.ZLinkFrameworkRuntimeState.Relocating, sealed: false },
  { state: framework.ZLinkFrameworkRuntimeState.Relocating, sealed: true }
]) for (const readyIntent of [false, true]) for (const send of [false, true]) {
  test(`Close release terminates ${readyIntent ? 'Ready' : 'Missing'} intent ${send ? 'send' : 'request'} in ${state} (sealed=${sealed}) without placement`, async () => {
    const expectation = branchExpectation('release-during-host-drain-or-relocation');
    const hostName = framework.ZLinkFrameworkRuntimeState[state];
    if (state === framework.ZLinkFrameworkRuntimeState.Relocating) {
      assert.ok(closeFixture.closeBranches.find((branch) => branch.name === 'release-during-host-drain-or-relocation')
        .given.relocationSeal.includes(sealed ? 'after' : 'before'));
    }
    const fixture = await authorityFixture();
    const ready = await commitFixture(fixture);
    let missingPlacementCalls = 0;
    const reserve = fixture.store.reserve.bind(fixture.store);
    fixture.store.reserve = (...args) => { missingPlacementCalls++; return reserve(...args); };
    const entered = deferred();
    const finish = deferred();
    const arrived = deferred();
    let initializations = 0;
    let reincarnations = 0;
    let releases = 0;
    let applicationReleases = 0;
    const failures = [];
    const terminalEvents = [];
    class Room {
      async onInitialize() { initializations++; }
      async onClosing() { entered.resolve(); await finish.promise; }
    }
    class Ping { handle() { assert.fail('released intent must never execute or be placed'); } }
    const claims = new ZLinkSpotLocationClaims({}, fixture.store);
    claims.trackInstanceAuthority({ meshName: 'mesh', spotId: 'close-room', stableType: 'room',
      nodeRid: 'node', nodeGeneration: 1n, objectGeneration: ready.objectGeneration,
      authorityOwnerGeneration: ready.authorityOwnerGeneration, ownerId: 'owner',
      ownerLeaseGeneration: 1n, storeVersion: ready.storeVersion.value });
    const manager = new framework.DefaultZLinkSpotManager({ detachedTaskRunner, spotFactories: [],
      instanceSpotFactories: new Map([['mesh', new Map([['room', Room]])]]),
      spotPacketHandlers: [{ spotType: Room, handlerType: Ping, packetName: 'Ping' }],
      statefulExecution: { admissionOpen: () => false, hostState: () => state },
      instanceSpotApplicationTargetProvider: () => ({ stableType: 'room', objectGeneration: ready.objectGeneration }),
      admission: { claim() { arrived.resolve(); return { close() { applicationReleases++; } }; } },
      dispatchErrors: { captureEnabled: () => true, flow: { flowCreationEnabled: () => false, accepts: () => false },
        report(event) {
          if (failures.length === 0) terminalEvents.push('pendingMessagesTerminated');
          failures.push(event);
        } },
      beginInstanceClosingAuthority: async (...args) => {
        const authority = await claims.beginInstanceClosing(...args);
        return { release: async () => {
          releases++;
          await authority.release();
          terminalEvents.push('authorityReleased');
        },
          reincarnate() { reincarnations++; assert.fail('host cannot reincarnate while draining or relocating'); } };
      }
    });
    await manager.materializeInstance('mesh', 'room', 'close-room', ready.objectGeneration);
    const closing = manager.close('mesh', 'close-room');
    await entered.promise;
    const activation = manager.relocationActivations('mesh')[0];
    const parts = protocol.encodeChannelEnvelopeParts(send ? 3 : 1, 'instance', 'Ping', {}).map((part) => zlink.Message.from(part));
    const replies = [];
    const record = { kind: framework.ReceiveKind.InstanceSpotActivation,
      operationKind: send ? 0 : framework.OperationKind.InstanceSpotRequest, parts,
      reply(replyParts) { replies.push(replyParts.map((part) => zlink.Message.from(part))); return zlink.SubmitResult.Ok; },
      activationRecord: { kind: 'instanceSpot', activation: readyIntent ? 'ready' : 'missing',
        ...(readyIntent ? { instanceIntent: true, route: { targetSpotId: 'close-room', targetNodeRid: 'node',
          targetNodeGeneration: 1n, objectGeneration: ready.objectGeneration, ownerId: 'owner',
          authorityOwnerGeneration: ready.authorityOwnerGeneration, leaseGeneration: 1n,
          storeVersion: ready.storeVersion.value } } : { target: { targetSpotId: 'close-room', stableType: 'room',
          targetNodeRid: 'node', targetNodeGeneration: 1n, descriptorVersion: '1' }, deadlineUnixMs: BigInt(Date.now() + 10_000) }),
        sourceNodeRid: 'source', sourceNodeGeneration: 1n, operationKind: send ? 'send' : 'request',
        operation: { high: 1n, low: 1n }, ...(send ? {} : { replyRouteId: 1n }) } };
    const dispatch = manager.dispatchMeshInstance('mesh', { spotId: 'close-room' }, record);
    await arrived.promise;
    const relocationSeal = sealed ? activation.sealExecution('relocation') : undefined;
    finish.resolve();
    await Promise.all([closing, dispatch]);
    await activation.serial.whenIdle();
    const expected = framework.ZLinkFrameworkErrorKind[expectation.messageTerminalByHost[hostName]];
    assert.equal(replies.length, send ? 0 : expectation.messageTerminalCount);
    if (!send) assert.throws(() => protocol.decodeChannelReply(replies[0]), (error) => error.kind === expected);
    assert.equal(failures.length, expectation.messageTerminalCount, 'released intent must report exactly once');
    const diagnostic = expectation.sendDiagnosticsByHost[hostName];
    for (const failure of failures) {
      assert.equal(failure.error.kind, framework.ZLinkFrameworkErrorKind[diagnostic.kind]);
      assert.equal(failure.surface, diagnostic.surface);
      assert.equal(failure.reason, diagnostic.reason);
    }
    assert.equal(applicationReleases, 1);
    assert.equal(initializations, 1);
    assert.equal(initializations - 1, expectation.thisHostFactoryCalls);
    assert.equal(reincarnations, 0);
    assert.equal(missingPlacementCalls, expectation.missingPlacementCalls);
    assert.equal(releases, 1);
    assert.equal((await fixture.store.readAuthority(fixture.key)).kind === 'missing' ? 'Missing' : 'other', expectation.authority);
    assert.deepEqual(terminalEvents, expectation.order);
    if (relocationSeal !== undefined) activation.commitExecutionSeal(relocationSeal);
    for (const part of [...parts, ...replies.flat()]) part.close();
  });
}

test('Instance intent request after the release decision is refused before admission with the owner fence', async () => {
  const expectation = branchExpectation('intent-after-release-decision-refused-before-admission');
  const { internalFrameworkErrorKindFromWireFailureCode } = require('../../packages/framework/dist/runtime/framework-errors-internal');
  const fixture = await authorityFixture();
  const ready = await commitFixture(fixture);
  const entered = deferred();
  const finish = deferred();
  const releaseRequested = deferred();
  const releaseHold = deferred();
  const arrived = deferred();
  const events = [];
  let initializations = 0;
  let handlerCalls = 0;
  class Room {
    async onInitialize() { initializations++; }
    async onClosing() { entered.resolve(); await finish.promise; }
  }
  class Ping { handle() { handlerCalls++; return { generation: 'old' }; } }
  const claims = new ZLinkSpotLocationClaims({}, fixture.store);
  claims.trackInstanceAuthority({ meshName: 'mesh', spotId: 'close-room', stableType: 'room',
    nodeRid: 'node', nodeGeneration: 1n, objectGeneration: ready.objectGeneration,
    authorityOwnerGeneration: ready.authorityOwnerGeneration, ownerId: 'owner',
    ownerLeaseGeneration: 1n, storeVersion: ready.storeVersion.value });
  const manager = new framework.DefaultZLinkSpotManager({ detachedTaskRunner, spotFactories: [],
    instanceSpotFactories: new Map([['mesh', new Map([['room', Room]])]]),
    spotPacketHandlers: [{ spotType: Room, handlerType: Ping, packetName: 'Ping' }],
    instanceSpotApplicationTargetProvider: () => ({ stableType: 'room', objectGeneration: ready.objectGeneration }),
    admission: { claim() { arrived.resolve(); return { close() {} }; } },
    beginInstanceClosingAuthority: async (...args) => {
      const authority = await claims.beginInstanceClosing(...args);
      return { release: async () => {
        // Close step 3 has decided release; the Store holds its Delete.
        releaseRequested.resolve();
        await releaseHold.promise;
        await authority.release();
        events.push('authorityReleased');
      }, reincarnate() { assert.fail('Close with no waiting Instance intent must release'); } };
    }
  });
  await manager.materializeInstance('mesh', 'room', 'close-room', ready.objectGeneration);
  const closing = manager.close('mesh', 'close-room');
  await entered.promise;
  finish.resolve();
  await releaseRequested.promise;
  const parts = protocol.encodeChannelEnvelopeParts(1, 'instance', 'Ping', {}).map((part) => zlink.Message.from(part));
  const replies = [];
  const failures = [];
  const record = { kind: framework.ReceiveKind.InstanceSpotActivation,
    operationKind: framework.OperationKind.InstanceSpotRequest, parts,
    reply(replyParts) { replies.push(replyParts.map((part) => zlink.Message.from(part))); return zlink.SubmitResult.Ok; },
    replyFailure(terminalResult, failureCode) { failures.push({ terminalResult, failureCode }); return zlink.SubmitResult.Ok; },
    activationRecord: { kind: 'instanceSpot', activation: 'ready', instanceIntent: true,
      route: { targetSpotId: 'close-room', targetNodeRid: 'node', targetNodeGeneration: 1n,
        objectGeneration: ready.objectGeneration, ownerId: 'owner',
        authorityOwnerGeneration: ready.authorityOwnerGeneration, leaseGeneration: 1n,
        storeVersion: ready.storeVersion.value },
      sourceNodeRid: 'source', sourceNodeGeneration: 1n, operationKind: 'request',
      operation: { high: 1n, low: 1n }, replyRouteId: 1n } };
  const dispatch = manager.dispatchMeshInstance('mesh', { spotId: 'close-room' }, record);
  // The late message is either refused at once or waits in the owner queue.
  await Promise.race([dispatch, arrived.promise]);
  releaseHold.resolve();
  assert.equal(await closing, true);
  await dispatch;
  assert.deepEqual(events, expectation.order);
  assert.equal((await fixture.store.readAuthority(fixture.key)).kind === 'missing' ? 'Missing' : 'other', expectation.authority);
  assert.equal(handlerCalls, expectation.oldHandlerCalls + expectation.newHandlerCalls);
  assert.equal(initializations - 1, expectation.factoryCalls);
  assert.equal(replies.length + failures.length, expectation.messageTerminalCount);
  assert.equal(replies.length, 0, 'the late Instance intent message must not be admitted');
  assert.equal(internalFrameworkErrorKindFromWireFailureCode(failures[0].failureCode), expectation.messageFailureCode);
  assert.equal(expectation.messageTerminal, 'Unavailable');
  for (const part of [...parts, ...replies.flat()]) part.close();
});

for (const { initializationFails, queuedBeforeClose, readyCommitFails, send, readyIntent } of [
  { initializationFails: false, queuedBeforeClose: false },
  { initializationFails: true, queuedBeforeClose: false },
  { initializationFails: false, queuedBeforeClose: true },
  { initializationFails: false, queuedBeforeClose: false, readyCommitFails: true },
  { initializationFails: false, queuedBeforeClose: false, send: true },
  { initializationFails: true, queuedBeforeClose: false, send: true },
  { initializationFails: false, queuedBeforeClose: false, readyCommitFails: true, send: true },
  { initializationFails: false, queuedBeforeClose: false, readyIntent: true },
  { initializationFails: false, queuedBeforeClose: true, readyIntent: true },
  { initializationFails: false, queuedBeforeClose: false, readyIntent: true, send: true }
]) {
  test(`Close retains ${readyIntent ? 'Ready intent' : 'Missing-original'} ${send ? 'send' : 'request'} until initialization (${initializationFails ? 'failure' : readyCommitFails ? 'ready-commit-failure' : queuedBeforeClose ? 'queued-before-close' : 'success'})`, async () => {
    const fixture = await authorityFixture();
    const ready = await commitFixture(fixture);
    const entered = deferred();
    const finish = deferred();
    const arrived = deferred();
    const initializing = deferred();
    const finishInitializing = deferred();
    const secondArrived = deferred();
    let admissionCount = 0;
    let releasedClaims = 0;
    let timerClears = 0;
    const timerCallbacks = new Set();
    const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
    const { runWithApplicationJobPermit } = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
    const jobs = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 1n));
    const permits = await Promise.all([jobs.acquire(undefined, 'remote'), jobs.acquire(undefined, 'remote'), ...(queuedBeforeClose ? [jobs.acquire(undefined, 'remote')] : [])]);
    const beforeHandler = permits.map(() => 0);
    const scopePermits = permits.map((permit, index) => ({
      releaseBeforeHandler() { beforeHandler[index]++; permit.releaseBeforeHandler(); },
      releaseAfterInternalProcessing() { permit.releaseAfterInternalProcessing(); }
    }));
    const events = [];
    const generations = [];
    let closingCalls = 0;
    let targetGeneration = ready.objectGeneration;
    class Room {
      async onInitialize() {
        events.push(`initialize:${this.context.objectGeneration}`);
        if (initializationFails && BigInt(this.context.objectGeneration) !== ready.objectGeneration) {
          throw new Error('new incarnation restore failed');
        }
        if (queuedBeforeClose && BigInt(this.context.objectGeneration) !== ready.objectGeneration) {
          initializing.resolve();
          await finishInitializing.promise;
        }
      }
      async onClosing() { closingCalls++; entered.resolve(); await finish.promise; }
    }
    class Tick { handle() { assert.fail('unexecuted old timer must be cancelled'); } }
    const timerClock = { now: () => 0, utcNow: () => Date.now(),
      setTimeout(callback) { timerCallbacks.add(callback); return callback; },
      clearTimeout(callback) { timerClears++; timerCallbacks.delete(callback); } };
    class Ping {
      handle(spot, request) {
        assert.equal(beforeHandler[request.id - 1], 1);
        assert.equal(jobs.snapshot().permitsInUse < BigInt(permits.length), true);
        generations.push(BigInt(spot.context.objectGeneration));
        events.push(`handler:${request.id}`);
        return { id: request.id, generation: spot.context.objectGeneration };
      }
    }
    const claims = new ZLinkSpotLocationClaims({}, fixture.store);
    claims.trackInstanceAuthority({ meshName: 'mesh', spotId: 'close-room', stableType: 'room',
      nodeRid: 'node', nodeGeneration: 1n, objectGeneration: ready.objectGeneration,
      authorityOwnerGeneration: ready.authorityOwnerGeneration, ownerId: 'owner',
      ownerLeaseGeneration: 1n, storeVersion: ready.storeVersion.value });
    const manager = new framework.DefaultZLinkSpotManager({ detachedTaskRunner, spotFactories: [],
      instanceSpotFactories: new Map([['mesh', new Map([['room', Room]])]]),
      spotPacketHandlers: [{ spotType: Room, handlerType: Ping, packetName: 'Ping' }],
      instanceSpotApplicationTargetProvider: () => ({ stableType: 'room', objectGeneration: targetGeneration }),
      spotTimerHandlers: [{ spotType: Room, handlerType: Tick, name: 'close-pending-timer', periodMs: 1_000 }],
      admission: { claim() { arrived.resolve(); if (++admissionCount === 3) secondArrived.resolve(); return { close() { releasedClaims++; } }; } },
      beginInstanceClosingAuthority: async (...args) => {
        const authority = await claims.beginInstanceClosing(...args);
        return { release: () => authority.release(), reincarnate: async (initialize) => {
          const next = await authority.reincarnate(initialize);
          targetGeneration = next.objectGeneration;
          return next;
        } };
      }
    }, timerClock);
    if (readyCommitFails) {
      const compare = fixture.store.compareExchangeAuthority.bind(fixture.store);
      fixture.store.compareExchangeAuthority = async (key, version, mutation) => {
        if (mutation.kind === 'put' && mutation.generationTransition === 'preserve' &&
          decodeServiceInstanceAuthorityPayload(mutation.payload)?.state === 'ready') {
          return { kind: 'conflict', current: await fixture.store.readAuthority(key) };
        }
        return await compare(key, version, mutation);
      };
    }
    await manager.materializeInstance('mesh', 'room', 'close-room', ready.objectGeneration);
    assert.equal(timerCallbacks.size > 0, true);
    const previousEntered = deferred();
    const previousFinish = deferred();
    const previous = queuedBeforeClose ? manager.executeOnSpot(Room, 'close-room', async () => {
      previousEntered.resolve();
      await previousFinish.promise;
    }) : undefined;
    if (previous !== undefined) await previousEntered.promise;
    if (queuedBeforeClose) for (const callback of [...timerCallbacks]) callback();
    const closing = manager.close('mesh', 'close-room');
    const closeOutcome = closing.then(() => 'closed', (error) => error.message);
    const queued = queuedBeforeClose ? receive(1, true) : undefined;
    previousFinish.resolve();
    await entered.promise;
    assert.equal(decodeServiceInstanceAuthorityPayload((await fixture.store.readAuthority(fixture.key)).payload).state, 'closing');
    function receive(id, intent) {
      const oneWay = send && intent;
      const parts = protocol.encodeChannelEnvelopeParts(oneWay ? 3 : 1, 'instance', 'Ping', { id }).map((part) => zlink.Message.from(part));
      const replies = [];
      const record = { kind: intent ? framework.ReceiveKind.InstanceSpotActivation : framework.ReceiveKind.SpotRequest,
        operationKind: oneWay ? 0 : framework.OperationKind.InstanceSpotRequest, parts,
        reply: (replyParts) => {
          replies.push(replyParts.map((part) => zlink.Message.from(part)));
          return zlink.SubmitResult.Ok;
        } };
      if (intent) record.activationRecord = { kind: 'instanceSpot', activation: 'missing',
        target: { targetSpotId: 'close-room', stableType: 'room', targetNodeRid: 'node', targetNodeGeneration: 1n, descriptorVersion: '1' },
        deadlineUnixMs: BigInt(Date.now() + 10_000),
        sourceNodeRid: 'source', sourceNodeGeneration: 1n, operationKind: oneWay ? 'send' : 'request',
        operation: { high: 1n, low: BigInt(id) }, ...(oneWay ? {} : { replyRouteId: BigInt(id) }) };
      if (readyIntent) {
        record.kind = framework.ReceiveKind.InstanceSpotActivation;
        record.activationRecord = { kind: 'instanceSpot', activation: 'ready', instanceIntent: intent,
          route: { targetSpotId: 'close-room', targetNodeRid: 'node', targetNodeGeneration: 1n,
            objectGeneration: ready.objectGeneration, ownerId: 'owner',
            authorityOwnerGeneration: ready.authorityOwnerGeneration, leaseGeneration: 1n,
            storeVersion: ready.storeVersion.value }, sourceNodeRid: 'source', sourceNodeGeneration: 1n,
          operationKind: oneWay ? 'send' : 'request', operation: oneWay ? { high: 0n, low: 0n } : { high: 1n, low: BigInt(id) },
          ...(oneWay ? {} : { replyRouteId: BigInt(id) }) };
      }
      const dispatch = runWithApplicationJobPermit(scopePermits[id - 1], () => intent || readyIntent ? manager.dispatchMeshInstance('mesh', { spotId: 'close-room' }, record)
        : manager.dispatchMeshSpot('mesh', { spotId: 'close-room' }, record));
      return { dispatch, parts, replies };
    }
    const intent = queued ?? receive(1, true);
    const noIntent = receive(2, false);
    await arrived.promise;
    finish.resolve();
    let second;
    let lateNoIntent;
    if (queuedBeforeClose) {
      await initializing.promise;
      second = receive(3, true);
      await secondArrived.promise;
      finishInitializing.resolve();
    }
    await Promise.all([previous, closeOutcome, intent.dispatch, noIntent.dispatch, second?.dispatch]);
    const expected = branchExpectation(initializationFails ? 'reincarnate-initialization-fails' : 'reincarnate-pending-intent');
    const noIntentExpected = branchExpectation('closing-message-without-intent');
    assert.equal(closingCalls, 1);
    assert.equal(intent.replies.length, send ? 0 : expected.messageTerminalCount);
    if (readyIntent && !send) {
      assert.doesNotThrow(() => protocol.decodeChannelReply(intent.replies[0]),
        'Ready Instance intent request must reply from the new incarnation instead of NotFound');
    }
    assert.equal(noIntent.replies.length, noIntentExpected.messageTerminalCount);
    assert.throws(() => protocol.decodeChannelReply(noIntent.replies[0]), (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound);
    assert.equal(events.includes('handler:2'), false);
    if (initializationFails || readyCommitFails) {
      assert.deepEqual(generations, []);
      assert.equal((await fixture.store.readAuthority(fixture.key)).kind, 'missing');
      if (!send) assert.throws(() => protocol.decodeChannelReply(intent.replies[0]), (error) => error.kind === framework.ZLinkFrameworkErrorKind[readyCommitFails ? 'Unavailable' : 'InternalFailure']);
      assert.equal(timerCallbacks.size, 0);
      assert.equal(timerClears >= 1, true);
    } else {
      assert.deepEqual(generations, queuedBeforeClose ? [targetGeneration, targetGeneration] : [targetGeneration]);
      if (second !== undefined) assert.equal(second.replies.length, 1);
      assert.notEqual(targetGeneration, ready.objectGeneration);
      assert.equal(events.indexOf(`initialize:${targetGeneration}`) < events.indexOf('handler:1'), true);
      assert.equal((await fixture.store.readAuthority(fixture.key)).objectGeneration, targetGeneration);
      if (readyIntent && !queuedBeforeClose && !send) {
        const permit = await jobs.acquire(undefined, 'remote');
        scopePermits.push({ releaseBeforeHandler() { assert.fail('stale no-intent Ready frame must not enter a handler'); },
          releaseAfterInternalProcessing() { permit.releaseAfterInternalProcessing(); } });
        lateNoIntent = receive(3, false);
        await lateNoIntent.dispatch;
        assert.equal(lateNoIntent.replies.length, 1);
        assert.throws(() => protocol.decodeChannelReply(lateNoIntent.replies[0]),
          (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound);
        assert.equal(events.includes('handler:3'), false);
      }
      const initializationCount = events.filter((event) => event.startsWith('initialize:')).length;
      await manager.close('mesh', 'close-room');
      assert.equal((await fixture.store.readAuthority(fixture.key)).kind, 'missing');
      assert.equal(events.filter((event) => event.startsWith('initialize:')).length - initializationCount,
        branchExpectation('release-without-pending-intent').factoryCalls);
    }
    assert.equal(jobs.snapshot().permitsInUse, 0n);
    assert.equal(releasedClaims, queuedBeforeClose || lateNoIntent !== undefined ? 3 : 2);
    for (const part of [...intent.parts, ...noIntent.parts, ...intent.replies.flat(), ...noIntent.replies.flat(),
      ...(second?.parts ?? []), ...(second?.replies.flat() ?? []),
      ...(lateNoIntent?.parts ?? []), ...(lateNoIntent?.replies.flat() ?? [])]) part.close();
  });
}


for (const targetMissing of [true, false]) {
  for (const request of [false, true]) {
    test(`Instance missing ${targetMissing ? 'target' : 'handler'} has one final ${request ? 'request' : 'send'} diagnostic`, async () => {
      const { ZLinkRoutedSpotPacketDispatch } = require('../../packages/framework/dist/runtime/spots/spot-routed-spot-packet-dispatch');
      const { dispatchReasonFromError } = require('../../packages/framework/dist/runtime/diagnostics/dispatch-error-details');
      const intermediate = [];
      const terminal = [];
      const finalReport = ( error ) => terminal.push({ error, reason: dispatchReasonFromError(error) });
      const dispatch = new ZLinkRoutedSpotPacketDispatch({
        resolveActivation: () => ( targetMissing ? undefined : { handlers: { snapshot: () => [] } }) ,
        dispatchErrors: { captureEnabled: () => true, report: ( event ) => intermediate.push(event) }
      });
      const context = { channelName: 'instance',
        activationRecord: { activationRecord: { kind: 'instanceSpot' } },
        onOneWayError: finalReport };
      if (request) {
        await assert.rejects(dispatch.request('missing-room', 'Ping', {}, context), ( error ) => {
          assert.ok(error instanceof framework.ZLinkConfigurationException);
          finalReport(error);
          return true;
        });
      } else {
        assert.equal(await dispatch.send('missing-room', 'Ping', {}, context), undefined);
      }
      assert.equal(intermediate.length, 0);
      assert.equal(terminal.length, 1);
      assert.equal(terminal[0].reason, request ? 'handler_exception' : 'no_handler');
      assert.equal(terminal[0].error.kind, request ? undefined : framework.ZLinkFrameworkErrorKind.NotFound);
    });
  }
}
