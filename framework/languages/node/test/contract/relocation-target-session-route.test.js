const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const { encodeServiceRelocationEnvelope } = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');
const { ZLinkNativeFallbackBoundSession } = require('../../packages/framework/dist/runtime/streams/native-fallback-bound-session');
const { ZLinkSubmitStatus } = require('../../packages/framework/dist/runtime/messaging/submission-result');

function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
}

function fixture() {
  let ready = false;
  const requested = deferred();
  const connected = deferred();
  const restored = deferred();
  const events = [];
  const binding = {
    participantId: 1n, sessionOwnerNodeRid: Buffer.from('session-owner'),
    sessionOwnerNodeGeneration: 7n, sessionOwnerId: 'session-owner',
    sessionOwnerLeaseGeneration: 8n, sessionRid: Buffer.from('session'),
    bindingGeneration: 9n, membershipEpoch: 1n
  };
  const envelope = {
    aggregateId: '11111111-1111-4111-8111-111111111111', aggregateGeneration: 1n,
    participants: [{ key: 'actor:a', objectKind: 'actor', stableType: 'actor',
      objectGeneration: 5n, authorityOwnerGeneration: 11n,
      applicationState: Buffer.alloc(0), boundSessionState: Buffer.alloc(0),
      sessionBinding: binding, queuedMessages: [], timers: [] }], memberships: []
  };
  const prepare = {
    kind: 'prepare', relocation: { high: 0x1111111111114111n, low: 0x8111111111111111n },
    targetAttemptGeneration: 1n,
    coordinator: { ownerId: 'source-owner', leaseGeneration: 3n, nodeRid: 'source', nodeGeneration: 2n, expectedAuthorityStoreVersion: 'v1' },
    target: { ownerId: 'target-owner', ownerLeaseGeneration: 4n, nodeRid: 'target', nodeGeneration: 6n },
    object: { kind: 'actor', actorId: 'a', objectGeneration: 5n, expectedAuthorityOwnerGeneration: 11n },
    initiatorRole: 'source', sourceNodeRid: 'source', sourceNodeGeneration: 2n,
    applicationVersion: 1n, deadlineUnixMs: BigInt(Date.now() + 10000)
  };
  const node = {
    status: () => ({ routingId: 'target', lifecycleGeneration: 6n }),
    isPeerRouteReady: () => ready,
    async waitForPeerRoute(peer, generation, deadline, signal) {
      assert.equal(String(peer), 'session-owner');
      assert.equal(generation, 7n);
      assert.equal(deadline, prepare.deadlineUnixMs);
      await connected.promise;
      signal?.throwIfAborted();
    }
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => node, currentOwner: () => ({ ownerId: 'target-owner', leaseGeneration: 4n }),
    spotManager: () => undefined,
    requestPeerReconcile: async (mesh, peer) => { events.push('reconcile'); requested.resolve(); }
  });
  runtime.targetAssemblies.set('attempt', { assembly: { fail() {}, payload: async () => encodeServiceRelocationEnvelope(envelope, 1n) } });
  runtime.resolveTargetParticipantInventory = async (_mesh, _request, decoded) => ({ ...decoded, participants: decoded.participants.map(p => ({ ...p, key: 'zla1:a:1:a', objectKind: 'actor', objectGeneration: 5n, authorityOwnerGeneration: 11n })) });
  runtime.materializeTargetEnvelope = async () => ({ owner: {
    async restoreSavedWork() { restored.resolve(); },
    async abort() { events.push('abort'); }
  }, staging: {}, target: {} });
  runtime.reserveTargetForPrepare = async () => ({ prepared: {} });
  const session = new ZLinkNativeFallbackBoundSession({
    runtime: {}, actorId: 'a', reportError() {}, flowCreationEnabled: () => false,
    actorRefProvider: () => undefined, nativeActorNodeProvider: () => undefined,
    localActorProvider: () => false,
    remoteBoundSessionTargetProvider: () => ({ routerChannelId: 'mesh', targetNodeRid: 'session-owner', spotId: 'entry' }),
    remoteActorPacketTargetProvider: () => undefined,
    routedTransport: { async submitInfrastructure() { return { status: ready ? ZLinkSubmitStatus.Submitted : ZLinkSubmitStatus.RouteNotConnected }; } }
  });
  return { runtime, prepare, requested, restored, events, session,
    connect() { ready = true; connected.resolve(); },
    prepareTarget() { return runtime.handlePrepareControl('mesh', 'attempt', prepare, 'source'); }
  };
}

test('target waits for Session owner route before READY and first boundSession push', async () => {
  const f = fixture();
  let terminal = false;
  const operation = f.prepareTarget().then(value => { terminal = true; return value; });
  await f.restored.promise;
  // Allow the continuation after Restore to reach the route check without a clock delay.
  await new Promise(resolve => setImmediate(resolve));
  if (terminal) await f.session.send({ value: 1 }).packetName('Push').submit();
  assert.equal(terminal, false, 'READY must wait for Session owner route');
  await f.requested.promise;
  f.connect();
  assert.equal((await operation).kind, 'ready');
  await f.session.send({ value: 1 }).packetName('Push').submit();
  assert.deepEqual(f.events, ['reconcile']);
});




for (const terminal of ['deadline', 'shutdown']) {
  test(`target ${terminal} rejects READY explicitly and discards restored staging`, async () => {
    const f = fixture();
    const errors = require('../../packages/framework/dist/contracts');
    const timeout = require('../../packages/framework/dist/runtime/foundation/operation-registry');
    let rejectRoute;
    f.runtime.options.meshNode().waitForPeerRoute = (_peer, _generation, _deadline, signal) =>
      new Promise((_resolve, reject) => {
        rejectRoute = reject;
        signal.addEventListener('abort', () => reject(signal.reason), { once: true });
      });
    const operation = f.prepareTarget();
    f.runtime.targetPrepareOperations.set('attempt', { promise: operation });
    let failed;
    const delivered = f.runtime.deliverTargetReady('mesh', 'attempt', f.prepare,
      { reply(bytes) {
        failed = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec').decodeMaintenanceRelocationControl(bytes);
        return require('../../packages/framework/dist/runtime/backend/runtime-values').SubmitResult.Ok;
      } }, operation);
    await f.requested.promise;
    if (terminal === 'deadline') rejectRoute(new timeout.OperationTimeoutError(1n));
    else await f.runtime.drainTargetAttempts();
    await delivered;
    assert.equal(failed.kind, 'failed');
    assert.equal(f.runtime.targetStages.size, 0);
    assert.equal(f.runtime.targetAssemblies.size, 0);
    assert.equal(f.events.filter(e => e === 'abort').length, 1);
    if (terminal === 'deadline') {
      await assert.rejects(operation, error => error.kind === errors.ZLinkFrameworkErrorKind.DeadlineExceeded);
    }
  });
}

test('target shutdown during payload assembly sends ShuttingDown and removes assembly state', async () => {
  const f = fixture();
  const { ZLinkRelocationPayloadAssembly } = require('../../packages/framework/dist/runtime/host/relocation-direct-transfer');
  f.runtime.targetAssemblies.set('attempt', { assembly: new ZLinkRelocationPayloadAssembly(1, 1, 0) });
  const operation = f.prepareTarget();
  f.runtime.targetPrepareOperations.set('attempt', { promise: operation });
  const failure = assert.rejects(operation, error => error.kind ===
    require('../../packages/framework/dist/contracts').ZLinkFrameworkErrorKind.ShuttingDown);
  await f.runtime.drainTargetAttempts();
  await failure;
  assert.equal(f.runtime.targetAssemblies.size, 0);
});

test('a ready notification after the source deadline cannot produce READY', async (t) => {
  let now = Date.now();
  t.mock.method(Date, 'now', () => now);
  const f = fixture();
  const operation = f.prepareTarget();
  await f.requested.promise;
  now += 10001;
  f.connect();
  await assert.rejects(operation, error => error.kind ===
    require('../../packages/framework/dist/contracts').ZLinkFrameworkErrorKind.DeadlineExceeded);
  assert.equal(f.runtime.targetStages.size, 0);
  assert.equal(f.events.includes('abort'), true);
});
