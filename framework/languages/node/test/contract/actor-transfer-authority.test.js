const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');

test('Core transfer-control phases drive durable prepare commit and activation', async () => {
  let nowMs = Date.UTC(2026, 6, 19, 0, 0, 0);
  const store = new framework.ZLinkInMemoryLocationStore(() => new Date(nowMs));
  await store.updateActor(actorLocation('alice'), framework.ZLinkLocationWriteIntent.NewClaim);
  const runtime = new framework.ZLinkActorTransferAuthorityRuntime({
    store: () => store,
    recoveryOwnerId: () => 'target-runtime',
    recoveryLeaseTtlMs: 30_000,
    now: () => new Date(nowMs)
  });
  const control = transferControl('alice');

  await runtime.handle('game', {
    ...control,
    phase: framework.ActorTransferPhase.Fenced,
    role: framework.ActorTransferRole.Target
  });
  assert.equal((await store.resolveActorTransfer('game', 'alice')).state, 'prepared');

  await runtime.handle('game', {
    ...control,
    phase: framework.ActorTransferPhase.Committed,
    role: framework.ActorTransferRole.Target
  });
  assert.equal((await store.resolveActorTransfer('game', 'alice')).state, 'committed');

  await runtime.handle('game', {
    ...control,
    phase: framework.ActorTransferPhase.Activated,
    role: framework.ActorTransferRole.Target
  });
  assert.equal(await store.resolveActorTransfer('game', 'alice'), undefined);
});

test('successor resumes committed recovery only after the durable lease expires', async () => {
  let nowMs = Date.UTC(2026, 6, 19, 0, 0, 0);
  const store = new framework.ZLinkInMemoryLocationStore(() => new Date(nowMs));
  await store.updateActor(actorLocation('bob'), framework.ZLinkLocationWriteIntent.NewClaim);
  const control = transferControl('bob', 2n);
  const first = new framework.ZLinkActorTransferAuthorityRuntime({
    store: () => store,
    recoveryOwnerId: () => 'failed-runtime',
    recoveryLeaseTtlMs: 100,
    now: () => new Date(nowMs)
  });
  await first.handle('game', {
    ...control,
    phase: framework.ActorTransferPhase.Fenced,
    role: framework.ActorTransferRole.Target
  });

  const successor = new framework.ZLinkActorTransferAuthorityRuntime({
    store: () => store,
    recoveryOwnerId: () => 'successor-runtime',
    recoveryLeaseTtlMs: 30_000,
    now: () => new Date(nowMs)
  });
  await successor.handle('game', {
    ...control,
    phase: framework.ActorTransferPhase.Activated,
    role: framework.ActorTransferRole.Target
  });
  assert.equal((await store.resolveActorTransfer('game', 'bob')).state, 'prepared');

  nowMs += 101;
  await successor.handle('game', {
    ...control,
    phase: framework.ActorTransferPhase.Activated,
    role: framework.ActorTransferRole.Target
  });
  assert.equal(await store.resolveActorTransfer('game', 'bob'), undefined);
});

test('framework host routes Core TransferControl records to the configured authority store', async () => {
  const registration = framework.createFrameworkRegistration({
    locations: { useInMemoryStores: true }
  });
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
  const store = host.locationOwner.locationStore();
  assert.ok(store);
  const sourceOwner = await store.claimOwnerLease('source-runtime', 30_000);
  assert.equal(sourceOwner.kind, 'claimed');
  await store.updateActor(
    actorLocation('carol', sourceOwner.token.leaseGeneration),
    framework.ZLinkLocationWriteIntent.NewClaim
  );
  host.locationRuntimeQuery;
  const control = transferControl('carol', 3n);

  await host.dispatchMeshRecord('game', { ownerKind: framework.ReadyOwnerKind.Node }, {
    kind: framework.ReceiveKind.TransferControl,
    kindData: {
      ...control,
      phase: framework.ActorTransferPhase.Fenced,
      role: framework.ActorTransferRole.Target
    },
    parts: []
  });

  assert.equal((await store.resolveActorTransfer('game', 'carol')).state, 'prepared');
});

function actorLocation(actorId, leaseGeneration = 1n) {
  return {
    meshName: 'game',
    actorId,
    actorType: 'player',
    actorRef: {
      nodeRid: rid('source'),
      actorId,
      objectGeneration: 7n,
      meshName: 'game'
    },
    ownerNodeRid: rid('source'),
    ownerNodeGeneration: 3n,
    spotKind: framework.ZLinkSpotKind.User,
    spotId: 'room-a',
    spotGeneration: 5n,
    membershipEpoch: 11n,
    ownerId: 'source-runtime',
    leaseGeneration,
    updatedAt: new Date(0)
  };
}

function transferControl(actorId, low = 1n) {
  return {
    kind: 'transferControl',
    phase: 0,
    role: 0,
    transferId: { high: 0x0123456789abcdefn, low },
    actor: {
      nodeRid: rid('target'),
      actorId,
      generation: 7n
    },
    membershipEpoch: 11n,
    finalSequence: 0n,
    resultCode: 0,
    failureErrno: 0
  };
}

function rid(value) {
  return zlink.RoutingId.from(value);
}
