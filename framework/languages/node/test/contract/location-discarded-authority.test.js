'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const flow = require('./helpers/telemetry-log-capture');
const { Module, Injectable, Scope } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nestjs = require('../../packages/nestjs/dist');
const internal = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');

function waitForNextHostOwnerRecovery(host) {
  const original = host.resumeOwnerLeaseOwnedWork.bind(host);
  let resolve;
  const completed = new Promise((done) => {
    resolve = done;
  });
  host.resumeOwnerLeaseOwnedWork = async (...args) => {
    try {
      return await original(...args);
    } finally {
      host.resumeOwnerLeaseOwnedWork = original;
      resolve();
    }
  };
  return completed;
}

for (const publicKind of ['actor', 'user_spot'])
  for (const episode of ['stale', 'outage-valid', 'outage-expired'])
    test(
      `discarded ${publicKind} authority is released before another node getOrCreate (${episode})`,
      { timeout: 15000 },
      async (t) => {
        let factories = 0;
        let currentRuntime;
        let hostNow = 100;
        t.mock.method(require('node:perf_hooks').performance, 'now', () => hostNow);
        const localDisposed = deferred();
        const owners = [];
        const trackExecution = internal.ZLinkLocationRuntime.prototype.trackExecution;
        t.mock.method(
          internal.ZLinkLocationRuntime.prototype,
          'trackExecution',
          function (...args) {
            owners.push(args[0]);
            return trackExecution.apply(this, args);
          }
        );
        const {
          ZLinkSpotActivationLifecycle
        } = require('../../packages/framework/dist/runtime/spots/spot-activation');
        const discardSpot = ZLinkSpotActivationLifecycle.prototype.discard;
        t.mock.method(ZLinkSpotActivationLifecycle.prototype, 'discard', async function (...args) {
          try {
            return await discardSpot.apply(this, args);
          } finally {
            localDisposed.resolve();
          }
        });
        const startRuntime = internal.ZLinkLocationRuntime.prototype.start;
        t.mock.method(internal.ZLinkLocationRuntime.prototype, 'start', async function (...args) {
          currentRuntime = this;
          return await startRuntime.apply(this, args);
        });
        class Room {
          constructor() {
            if (publicKind === 'user_spot') factories++;
          }
        }
        Injectable({ scope: Scope.TRANSIENT })(Room);
        class Player {
          constructor(context) {
            this.context = context;
          }
        }
        class Factory {
          async create(context) {
            factories++;
            return new Player(context);
          }
        }
        class Adapter {
          async capture() {
            return new Uint8Array();
          }
          async restore() {}
        }
        class Entry {}
        let storeUtc = Date.now(),
          unavailable = false,
          providerIo = 0;
        const provider = new internal.ZLinkInMemoryProviderLocationStore(() => new Date(storeUtc));
        for (const name of ['read', 'scan', 'write']) {
          const operation = provider[name].bind(provider);
          provider[name] = (...args) => {
            providerIo++;
            if (unavailable) return Promise.reject(new Error('Store unavailable'));
            return operation(...args);
          };
        }
        async function start() {
          const builder = nestjs.zlinkFramework().addLocationStore(provider);
          builder.configureDispatch().messageFlow('detailed');
          builder.configureLocations().ownerLeaseFencingMarginMs(1).storeOutageRetention(true);
          builder
            .addRouteMesh('discarded')
            .listen('tcp://127.0.0.1:0')
            .objects()
            .server()
            .addEntrySpot(Entry, 'player')
            .addActorFactory('player', Factory, (factory) => factory.preserveStateWith(Adapter))
            .addSpotFactory('room', Room, (factory) => factory.preserveStateWith(Adapter));
          class App {}
          Module({
            imports: [nestjs.ZLinkModule.forRoot(builder.build())],
            providers: [Factory, Adapter, Room]
          })(App);
          const app = await NestFactory.createApplicationContext(App, {
            logger: false,
            abortOnError: false
          });
          t.after(() => app.close());
          return {
            app,
            runtime: currentRuntime,
            host: app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME),
            actors: app.get(nestjs.ZLINK_ACTOR_MANAGER),
            spots: app.get(nestjs.ZLINK_SPOT_MANAGER)
          };
        }
        t.after(() => flow.close());
        const a = await start();
        const create = (node) =>
          publicKind === 'actor'
            ? node.actors.getOrCreate('discarded-player', 'player').inMesh('discarded').submit()
            : node.spots.getOrCreate('discarded-room', 'room').inMesh('discarded').submit();
        const created = await create(a);
        assert.ok(created.actor ?? created.spot);
        const runtime = a.runtime;
        const store = new internal.ZLinkLocationStoreRepository(provider);
        const key = encodeAuthorityKey(
          publicKind,
          publicKind === 'actor' ? 'discarded-player' : 'discarded-room'
        );
        const before = await store.readAuthority(key);
        assert.equal(before.kind, 'snapshot');
        if (episode === 'stale') await store.releaseOwnerLease(runtime.currentOwnerToken);
        else {
          unavailable = true;
          assert.equal(await runtime.renewOwnerLeaseOnce(), false);
          const {
            inspectRuntime
          } = require('../../packages/framework/dist/runtime/internal-observation');
          hostNow = runtime[inspectRuntime]().storeOutage.cutoffMs;
          const beforeDiscardIo = providerIo;
          assert.throws(() => runtime.executionAllowed(owners[0]), {
            kind: internal.ZLinkFrameworkErrorKind.NotFound
          });
          assert.equal(providerIo, beforeDiscardIo);
          unavailable = false;
          if (episode === 'outage-expired') storeUtc += 30001;
        }
        const recovery = waitForNextHostOwnerRecovery(a.host);
        assert.equal(await runtime.renewOwnerLeaseOnce(), true);
        await recovery;
        if (publicKind === 'user_spot') await localDisposed.promise;
        await runtime.reclaimOwnerAuthorities();
        if (episode !== 'outage-valid') {
          assert.equal((await store.readAuthority(key)).kind, 'snapshot');
          hostNow += 3;
          assert.equal(await runtime.renewOwnerLeaseOnce(), true);
          await runtime.reclaimOwnerAuthorities();
        }
        assert.equal(
          (await store.readAuthority(key)).kind,
          'missing',
          'discarded authority must be cleaned before a new creation request'
        );
        const capacities = await store.listMeshNodes('discarded');
        assert.equal(
          capacities.items.find((value) => value.ownerId === before.ownerId).populationCapacity[
            publicKind === 'actor' ? 'actors' : 'spots'
          ].active,
          0
        );
        const b = await start();
        const recreated = await create(b);
        assert.ok(recreated.actor ?? recreated.spot);
        const after = await store.readAuthority(key);
        assert.equal(after.kind, 'snapshot');
        assert.notEqual(after.objectGeneration, before.objectGeneration);
        assert.notEqual(after.ownerId, before.ownerId);
        assert.equal(factories, 2);
      }
    );
const { RoutingId } = require('@zlink-systems/zlink');
const {
  publishInitialActorAuthority,
  rewriteActorAuthorityRoute
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  encodeServiceUserSpotAuthorityPayload,
  encodeServiceInstanceAuthorityPayload
} = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');
function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
async function cleanupFixture(
  t,
  provider,
  kind = 'actor',
  capacity = 10,
  onDiscard,
  retention = true
) {
  let now = 100,
    utc = 0;
  const timers = new Set();
  const date = () => new Date(utc);
  const backing = new internal.ZLinkInMemoryProviderLocationStore(date);
  const store = provider
    ? new internal.ZLinkLocationStoreRepository(backing, date, undefined, () => now, 100)
    : new internal.ZLinkInMemoryLocationStore(date, () => now, 100);
  const lease = Object.fromEntries(
    ['claimOwnerLease', 'renewOwnerLease', 'readOwnerLease', 'releaseOwnerLease'].map((name) => [
      name,
      (...args) => store[name](...args)
    ])
  );
  const runtime = new internal.ZLinkLocationRuntime({
    ownerId: 'recovery-owner',
    stores: {
      locationStore: store,
      authorityStore: store,
      ownerLeaseStore: lease,
      actorStore: store,
      spotStore: store,
      routeStore: store,
      peerStore: store
    },
    options: {
      storeOutageRetention: retention,
      ownerLeaseTtlMs: 1000,
      ownerLeaseFencingMarginMs: 100,
      ownerLeaseRenewTimeoutMs: 50
    },
    monotonicNowMs: () => now,
    setTimer(callback, delay) {
      const timer = { callback, at: now + delay };
      timers.add(timer);
      return timer;
    },
    clearTimer(timer) {
      timers.delete(timer);
    }
  });
  const rid = RoutingId.from('recovery-node');
  await runtime.start(rid);
  const lifecycle = new internal.ZLinkLocationLifecycle(runtime, store, 'play', store, store);
  const descriptor = (owner) => ({
    meshName: 'play',
    rid,
    lifecycleGeneration: 1n,
    descriptorRevision: owner.leaseGeneration,
    endpoint: 'tcp://127.0.0.1:1',
    objectRole: internal.ZLinkObjectRole.Server,
    placementWeight: 1,
    populationCapacity: {
      actors: { active: 0, reserved: 0, limit: capacity },
      spots: { active: 0, reserved: 0, limit: capacity },
      spotTypes: []
    },
    activationConcurrency: { active: 0, limit: 10 },
    channelWeights: {},
    applicationVersion: 1n,
    spotTypes: [],
    objectCapabilities: [],
    state: internal.ZLinkFrameworkRuntimeState.Serving,
    securityIdentity: 'test',
    ownerId: owner.ownerId,
    leaseGeneration: owner.leaseGeneration,
    updatedAt: date()
  });
  const originalOwner = runtime.currentOwnerToken;
  await assert.rejects(
    store.updateMeshNode(
      descriptor(originalOwner),
      internal.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
    ),
    (error) => error.kind === internal.ZLinkFrameworkErrorKind.Unavailable
  );
  now = 400;
  assert.equal(
    (
      await store.updateMeshNode(
        descriptor(originalOwner),
        internal.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
      )
    ).status,
    'stored'
  );
  const id = kind === 'actor' ? 'recovery-actor' : 'recovery-spot';
  const key = encodeAuthorityKey(kind, id);
  let authority;
  if (kind === 'actor') {
    await lifecycle.claimActor('player', id, rid);
    const actor = { actorId: id, objectGeneration: 1n, meshName: 'play', nodeRid: rid };
    await lifecycle.setActorRef('player', id, actor, 1n);
    authority = await publishInitialActorAuthority(store, {
      actorType: 'player',
      actor,
      meshName: 'play',
      ownerNodeGeneration: 1n,
      owner: originalOwner,
      spotId: String(rid),
      spotGeneration: 1n,
      spotKind: internal.ZLinkSpotKind.Entry
    });
  } else {
    const spotId = id;
    if (kind === 'user_spot')
      await lifecycle.claimSpot('play', spotId, 'game', rid, internal.ZLinkSpotKind.User, 1n, 1n);
    const target = {
      meshName: 'play',
      nodeRid: rid,
      nodeLifecycleGeneration: 1n,
      owner: originalOwner
    };
    const reserved = await store.reserve({
      key: { kind, globalId: id },
      intent: {
        stableType: 'game',
        requestContentReference: 'recovery-test',
        requestSha256: Buffer.alloc(32, 1),
        requestEncodedSize: 1n
      },
      target,
      capacity: { actors: 0, spots: 1 },
      creatingPayload: Buffer.from([1])
    });
    assert.equal(reserved.kind, 'reserved');
    const committed = await store.commit({
      key: { kind, globalId: id },
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target,
      readyPayload: (kind === 'user_spot'
        ? encodeServiceUserSpotAuthorityPayload
        : encodeServiceInstanceAuthorityPayload)({
        state: 'ready',
        stableType: 'game',
        spotId: id,
        ownerId: originalOwner.ownerId,
        ownerLeaseGeneration: originalOwner.leaseGeneration,
        ownerMeshName: 'play',
        ownerNodeRid: String(rid),
        ownerNodeGeneration: 1n
      })
    });
    assert.equal(committed.kind, 'committed');
    authority = committed.ready;
    if (kind === 'instance_spot')
      lifecycle.trackInstanceSpot({
        meshName: 'play',
        spotId: id,
        stableType: 'game',
        nodeRid: rid,
        nodeGeneration: 1n,
        objectGeneration: authority.objectGeneration,
        authorityOwnerGeneration: authority.authorityOwnerGeneration,
        ownerId: originalOwner.ownerId,
        ownerLeaseGeneration: originalOwner.leaseGeneration,
        storeVersion: authority.storeVersion.value
      });
  }
  lifecycle.confirmAuthority?.(key, authority);
  const execution = { queue: [], timer: {}, binding: { identity: {}, sequence: 7n } };
  let discarded = 0;
  if (kind === 'actor') lifecycle.bindActorExecution(id, execution);
  else lifecycle.bindSpotExecution('play', id, execution);
  lifecycle.trackExecution(
    execution,
    () => true,
    (error) => {
      discarded++;
      return onDiscard?.(error);
    }
  );
  const cas = store.compareExchangeAuthority.bind(store);
  t.after(async () => {
    lifecycle.dispose();
    await runtime.stop();
    assert.equal(timers.size, 0);
  });
  return {
    clock: {
      now: () => now,
      utcNow: () => now,
      setTimeout(callback, delay) {
        const timer = { callback, at: now + delay };
        timers.add(timer);
        return timer;
      },
      clearTimeout(timer) {
        timers.delete(timer);
      }
    },
    store,
    backing,
    descriptor,
    runtime,
    lifecycle,
    execution,
    key,
    authority,
    originalOwner,
    cas,
    discarded: () => discarded,
    async addActor(actorId, objectGeneration = 2n, onDiscard) {
      const actor = { actorId, objectGeneration, meshName: 'play', nodeRid: rid };
      assert.equal((await lifecycle.claimActor('player', actorId, rid)).status, 'claimed');
      await lifecycle.setActorRef('player', actorId, actor, 1n);
      const snapshot = await publishInitialActorAuthority(store, {
        actorType: 'player',
        actor,
        meshName: 'play',
        ownerNodeGeneration: 1n,
        owner: runtime.currentOwnerToken,
        spotId: String(rid),
        spotGeneration: 1n,
        spotKind: internal.ZLinkSpotKind.Entry
      });
      const actorKey = encodeAuthorityKey('actor', actorId);
      lifecycle.confirmAuthority?.(actorKey, snapshot);
      const owner = {};
      let disposed = 0;
      lifecycle.bindActorExecution(actorId, owner);
      lifecycle.trackExecution(
        owner,
        () => true,
        () => {
          disposed++;
          return onDiscard?.();
        }
      );
      return { key: actorKey, execution: owner, discarded: () => disposed };
    },
    setNow(value) {
      now = value;
    },
    async outage(at = 200) {
      now = at;
      lease.renewOwnerLease = async () => {
        throw new Error('Store unavailable');
      };
      assert.equal(await runtime.renewOwnerLeaseOnce(), false);
    },
    async renew(expired = false, at = expired ? 1200 : 300) {
      now = at;
      if (expired) utc = 1001;
      lease.renewOwnerLease = (...args) => store.renewOwnerLease(...args);
      return runtime.renewOwnerLeaseOnce();
    },
    tick(value) {
      now = value;
      for (const timer of [...timers])
        if (timer.at <= now) {
          timers.delete(timer);
          timer.callback();
        }
    },
    expire() {
      utc += 1001;
    }
  };
}

for (const provider of [false, true])
  for (const kind of ['actor', 'user_spot']) {
    test(`discard cleanup waits exactly 3 margins after stale without outage (${provider}/${kind})`, async (t) => {
      const s = await cleanupFixture(t, provider, kind);
      await s.store.releaseOwnerLease(s.originalOwner);
      assert.equal(await s.renew(false, 500), true);
      assert.equal(s.discarded(), 1);
      await s.runtime.reclaimOwnerAuthorities();
      assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
      s.setNow(799);
      await s.runtime.reclaimOwnerAuthorities();
      assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
      s.setNow(800);
      await s.runtime.reclaimOwnerAuthorities();
      assert.equal((await s.store.readAuthority(s.key)).kind, 'missing');
    });

    for (const expired of [false, true])
      test(`outage discard performs no cleanup I/O and cleans after recovery (${provider}/${kind}/${expired})`, async (t) => {
        const s = await cleanupFixture(t, provider, kind);
        await s.outage(500);
        let cleanupReads = 0;
        let providerIo = 0;
        for (const name of ['read', 'scan', 'write']) {
          const operation = s.backing[name].bind(s.backing);
          s.backing[name] = (...args) => {
            providerIo++;
            return operation(...args);
          };
        }
        const read = s.store.readAuthority.bind(s.store);
        s.store.readAuthority = (...args) => {
          cleanupReads++;
          return read(...args);
        };
        s.tick(1300);
        assert.equal(s.discarded(), 1);
        assert.equal(cleanupReads, 0);
        assert.equal(providerIo, 0);
        assert.equal((await read(s.key)).kind, 'snapshot');
        assert.equal(await s.renew(expired, 1400), true);
        await s.runtime.reclaimOwnerAuthorities();
        if (expired) {
          assert.equal((await read(s.key)).kind, 'snapshot');
          s.setNow(1699);
          await s.runtime.reclaimOwnerAuthorities();
          assert.equal((await read(s.key)).kind, 'snapshot');
          s.setNow(1700);
          await s.runtime.reclaimOwnerAuthorities();
        }
        assert.equal((await read(s.key)).kind, 'missing');
      });

    test(`discard cleanup waits for local disposal continuation (${provider}/${kind})`, async (t) => {
      const settled = deferred();
      const s = await cleanupFixture(t, provider, kind, 10, () => settled.promise);
      s.runtime.discardExecution(s.execution);
      await s.runtime.reclaimOwnerAuthorities();
      assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
      settled.resolve();
      await Promise.resolve();
      await s.runtime.reclaimOwnerAuthorities();
      assert.equal((await s.store.readAuthority(s.key)).kind, 'missing');
    });

    test(`discard cleanup preserves another incarnation (${provider}/${kind})`, async (t) => {
      const s = await cleanupFixture(t, provider, kind);
      s.runtime.discardExecution(s.execution);
      const changed = await s.cas(s.key, s.authority.storeVersion, {
        kind: 'put',
        generationTransition: 'reincarnate',
        payload: s.authority.payload
      });
      assert.equal(changed.kind, 'stored');
      await s.runtime.reclaimOwnerAuthorities();
      const after = await s.store.readAuthority(s.key);
      assert.equal(after.kind, 'snapshot');
      assert.equal(after.objectGeneration, changed.objectGeneration);
    });
  }

test('discarded User Spot membership snapshot reads only Actor authorities', async (t) => {
  const s = await cleanupFixture(t, true, 'user_spot');
  await s.addActor('unrelated-actor');
  s.runtime.discardExecution(s.execution);
  const scan = s.store.listAuthorities.bind(s.store);
  const prefixes = [];
  let items = 0;
  s.store.listAuthorities = async (prefix, cursor, limit, signal) => {
    prefixes.push(prefix);
    const page = await scan(prefix, cursor, limit, signal);
    if (page.kind === 'page') items += page.items.length;
    return page;
  };
  await s.runtime.reclaimOwnerAuthorities();
  assert.equal((await s.store.readAuthority(s.key)).kind, 'missing');
  assert.deepEqual(prefixes, ['zla1:a:']);
  assert.equal(items, 1);
});

test('discarded User Spot retains another node member and does not treat failed or expired scans as empty', async (t) => {
  const s = await cleanupFixture(t, true, 'user_spot');
  await s.addActor('first-nonmember');
  const remoteLease = await s.store.claimOwnerLease('remote-owner', 1000);
  assert.equal(remoteLease.kind, 'claimed');
  const remoteRid = RoutingId.from('remote-node');
  const remoteDescriptor = { ...s.descriptor(remoteLease.token), rid: remoteRid };
  await assert.rejects(
    s.store.updateMeshNode(
      remoteDescriptor,
      internal.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
    ),
    (error) => error.kind === internal.ZLinkFrameworkErrorKind.Unavailable
  );
  s.setNow(700);
  assert.equal(
    (
      await s.store.updateMeshNode(
        remoteDescriptor,
        internal.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim
      )
    ).status,
    'stored'
  );
  const memberKey = encodeAuthorityKey('actor', 'remote-member');
  await publishInitialActorAuthority(s.store, {
    actorType: 'player',
    actor: { actorId: 'remote-member', objectGeneration: 3n, meshName: 'play', nodeRid: remoteRid },
    meshName: 'play',
    ownerNodeGeneration: 1n,
    owner: remoteLease.token,
    spotId: 'recovery-spot',
    spotGeneration: s.authority.objectGeneration,
    spotKind: internal.ZLinkSpotKind.User
  });
  s.runtime.discardExecution(s.execution);
  await s.runtime.reclaimOwnerAuthorities();
  assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
  assert.equal((await s.store.readAuthority(memberKey)).ownerId, 'remote-owner');
  const scan = s.store.listAuthorities.bind(s.store);
  s.store.listAuthorities = async () => {
    throw new Error('membership snapshot unavailable');
  };
  await assert.rejects(s.runtime.reclaimOwnerAuthorities(), /membership snapshot unavailable/);
  assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
  let restarted = false,
    starts = 0;
  s.store.listAuthorities = async (prefix, cursor, _limit, signal) => {
    if (cursor === undefined) starts++;
    if (cursor !== undefined && !restarted) {
      restarted = true;
      return { kind: 'scanExpired' };
    }
    return scan(prefix, cursor, 1, signal);
  };
  await s.runtime.reclaimOwnerAuthorities();
  assert.equal(restarted, true);
  assert.equal(starts, 2);
  assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
  assert.equal((await s.store.readAuthority(memberKey)).ownerId, 'remote-owner');
});

for (const pendingKind of ['local', 'target-admission'])
  test(`Spot cleanup waits for accepted ${pendingKind} operation settlement`, async (t) => {
    const s = await cleanupFixture(t, true, 'actor');
    const disposed = deferred();
    const {
      ZLinkSpotActivationLifecycle
    } = require('../../packages/framework/dist/runtime/spots/spot-activation');
    const discard = ZLinkSpotActivationLifecycle.prototype.discard;
    t.mock.method(ZLinkSpotActivationLifecycle.prototype, 'discard', async function (...args) {
      try {
        return await discard.apply(this, args);
      } finally {
        disposed.resolve();
      }
    });
    const entered = deferred(),
      released = deferred();
    class Room {
      async onActorJoin() {
        if (pendingKind === 'target-admission') {
          entered.resolve();
          await released.promise;
        }
        return { accepted: true };
      }
    }
    const tasks = [];
    const detachedTaskRunner = {
      runDetached(_name, task) {
        tasks.push(Promise.resolve().then(task));
      }
    };
    const admission = new internal.ZLinkRuntimeAdmissionGate(() => s.runtime.ownerLeaseUsable);
    admission.setExecutionRuntime(() => s.runtime);
    admission.register('play');
    const spots = new internal.DefaultZLinkSpotManager(
      {
        spotFactories: [Room],
        admission,
        locationLifecycle: s.lifecycle,
        nodeRid: 'recovery-node',
        nodeRidProvider: () => 'recovery-node',
        nodeGenerationProvider: () => 1n,
        createNativeSpot: (_mesh, rid) => ({
          routingId: rid,
          lifecycleGeneration: 1n,
          async dispose() {}
        }),
        createTopicMessage: () => ({}),
        detachedTaskRunner,
        canonicalActorJoinResolver: async () => ({ actorType: 'player' })
      },
      s.clock
    );
    const created = await spots.create('play', Room, {});
    const activation = spots.activations.activationForClose('play', created.spotId);
    assert.ok(activation);
    const target = {
      meshName: 'play',
      nodeRid: RoutingId.from('recovery-node'),
      nodeLifecycleGeneration: 1n,
      owner: s.originalOwner
    };
    const key = encodeAuthorityKey('user_spot', String(created.spotId));
    const reserved = await s.store.reserve({
      key: { kind: 'user_spot', globalId: String(created.spotId) },
      intent: {
        stableType: 'room',
        requestContentReference: 'join-test',
        requestSha256: Buffer.alloc(32, 1),
        requestEncodedSize: 1n
      },
      target,
      capacity: { actors: 0, spots: 1 },
      creatingPayload: Buffer.from([1])
    });
    assert.equal(reserved.kind, 'reserved');
    const committed = await s.store.commit({
      key: { kind: 'user_spot', globalId: String(created.spotId) },
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target,
      readyPayload: encodeServiceUserSpotAuthorityPayload({
        state: 'ready',
        stableType: 'room',
        spotId: String(created.spotId),
        ownerId: s.originalOwner.ownerId,
        ownerLeaseGeneration: s.originalOwner.leaseGeneration,
        ownerMeshName: 'play',
        ownerNodeRid: 'recovery-node',
        ownerNodeGeneration: 1n
      })
    });
    assert.equal(committed.kind, 'committed');
    s.lifecycle.confirmAuthority(key, committed.ready);
    const write = s.backing.write.bind(s.backing);
    let held = false;
    s.backing.write = async (request, signal) => {
      if (
        pendingKind === 'local' &&
        !held &&
        request.mutations.some(
          (item) =>
            item.kind === 'put' && item.key.value.includes('authority\0actor\0recovery-actor')
        )
      ) {
        held = true;
        entered.resolve();
        await released.promise;
      }
      return write(request, signal);
    };
    const {
      ZLinkSpotActorMembership
    } = require('../../packages/framework/dist/runtime/spots/spot-actor-membership');
    const {
      ZLinkBufferMessage
    } = require('../../packages/framework/dist/runtime/backend/runtime-message');
    const membership = new ZLinkSpotActorMembership({ resolveActivation: () => activation });
    const actor = s.execution;
    const {
      ZLINK_ACTOR_LIFECYCLE_SNAPSHOT
    } = require('../../packages/framework/dist/runtime/actors/actor-lifecycle-snapshot');
    actor.context = {
      actorId: 'recovery-actor',
      [ZLINK_ACTOR_LIFECYCLE_SNAPSHOT]: () => ({
        actorRef: {
          actorId: 'recovery-actor',
          objectGeneration: s.authority.objectGeneration,
          meshName: 'play',
          nodeRid: target.nodeRid
        },
        actorType: 'player',
        membershipEpoch: 0n
      })
    };
    const request = ZLinkBufferMessage.from(Buffer.alloc(0));
    t.after(() => request.close());
    const joining =
      pendingKind === 'target-admission'
        ? spots.dispatchMeshActorJoin(
            'play',
            { spotId: created.spotId, actor: null },
            {
              kind: internal.ReceiveKind.SpotControl,
              kindData: {
                kind: 'actorControl',
                currentActor: {
                  actorId: 'recovery-actor',
                  nodeRid: target.nodeRid,
                  generation: s.authority.objectGeneration
                },
                previousActor: null,
                currentSpotGeneration: committed.ready.objectGeneration,
                currentMembershipEpoch: 1n,
                canonicalActorJoin: {
                  handoffId: 'discard-target-attempt',
                  actorNodeRid: String(target.nodeRid),
                  actorGeneration: s.authority.objectGeneration,
                  actorNodeGeneration: 1n,
                  authorityOwnerGeneration: s.authority.authorityOwnerGeneration,
                  ownerLeaseGeneration: s.originalOwner.leaseGeneration
                }
              },
              parts: [request],
              replyActorJoin() {
                throw new Error('A discarded admission must not publish acceptance');
              }
            }
          )
        : membership.admitActorJoin(created.spotId, actor, request, async () => {
            const payload = rewriteActorAuthorityRoute(
              s.authority.payload,
              {
                actorId: 'recovery-actor',
                objectGeneration: s.authority.objectGeneration,
                meshName: 'play',
                nodeRid: target.nodeRid
              },
              String(created.spotId),
              committed.ready.objectGeneration,
              internal.ZLinkSpotKind.User
            );
            const stored = await s.cas(s.key, s.authority.storeVersion, {
              kind: 'put',
              generationTransition: 'preserve',
              payload
            });
            assert.equal(stored.kind, 'stored');
            s.lifecycle.confirmAuthority(s.key, { ...stored, kind: 'snapshot' });
          });
    const joinOutcome = joining.then(
      (value) => ({ value }),
      (error) => ({ error })
    );
    await entered.promise;
    s.runtime.discardExecution(activation.spot);
    await s.runtime.reclaimOwnerAuthorities();
    assert.equal((await s.store.readAuthority(key)).kind, 'snapshot');
    released.resolve();
    const outcome = await joinOutcome;
    if (outcome.error !== undefined)
      assert.equal(outcome.error.kind, internal.ZLinkFrameworkErrorKind.NotFound);
    else if (pendingKind === 'local') assert.equal(outcome.value.accepted, true);
    await activation.serial.whenIdle();
    await disposed.promise;
    await s.runtime.reclaimOwnerAuthorities();
    await s.runtime.reclaimOwnerAuthorities();
    if (pendingKind === 'local') {
      assert.equal(s.discarded(), 1);
      assert.equal((await s.store.readAuthority(s.key)).kind, 'missing');
    } else {
      assert.equal(
        spots.formalRemoteActorAdmissions.get('discard-target-attempt').state,
        'aborted'
      );
      assert.equal(s.discarded(), 0);
      assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
    }
    assert.equal((await s.store.readAuthority(key)).kind, 'missing');
    await Promise.all(tasks);
  });

for (const failure of ['throw', 'reject'])
  test(`failed local disposal keeps cleanup pending without closing owner admission (${failure})`, async (t) => {
    const s = await cleanupFixture(t, true, 'actor', 10, () => {
      const error = new Error('local disposal failed');
      if (failure === 'throw') throw error;
      return Promise.reject(error);
    });
    s.runtime.discardExecution(s.execution);
    await Promise.resolve();
    await s.runtime.reclaimOwnerAuthorities();
    assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
    assert.equal(s.runtime.ownerLeaseUsable, true);
  });

for (const expired of [false, true])
  test(`discard cleanup confirms a lost applied response without returning capacity twice (${expired})`, async (t) => {
    const s = await cleanupFixture(t, true, 'actor');
    if (expired) {
      await s.store.releaseOwnerLease(s.originalOwner);
      assert.equal(await s.renew(false, 500), true);
      await s.runtime.reclaimOwnerAuthorities();
      s.setNow(800);
    } else s.runtime.discardExecution(s.execution);
    const write = s.backing.write.bind(s.backing);
    let lost = false;
    s.backing.write = async (request, signal) => {
      const result = await write(request, signal);
      if (
        !lost &&
        result.kind === 'applied' &&
        request.mutations.some(
          (item) =>
            item.kind === 'delete' && item.key.value.includes('authority\0actor\0recovery-actor')
        )
      ) {
        lost = true;
        throw new Error('applied cleanup response lost');
      }
      return result;
    };
    await assert.rejects(
      s.runtime.reclaimOwnerAuthorities(),
      (error) =>
        error.kind === internal.ZLinkFrameworkErrorKind.Unavailable &&
        error.cause?.message === 'applied cleanup response lost'
    );
    assert.equal(lost, true);
    assert.equal((await s.store.readAuthority(s.key)).kind, 'missing');
    await s.runtime.reclaimOwnerAuthorities();
    const capacity = await s.backing.scan({ prefix: 'zlink:v11:capacity:', limit: 10 });
    assert.equal(capacity.kind, 'page');
    for (const row of capacity.value.items)
      assert.equal(JSON.parse(Buffer.from(row.value.bytes)).active.actors, 0);
  });

for (const pendingPhase of ['local', 'store'])
  test(`Spot membership snapshot waits for its Actor ${pendingPhase} cleanup while an unrelated Spot proceeds`, async (t) => {
    const s = await cleanupFixture(t, true, 'user_spot');
    const settled = deferred();
    const member = await s.addActor('dependent-member', 2n, () =>
      pendingPhase === 'local' ? settled.promise : undefined
    );
    const authority = await s.store.readAuthority(member.key);
    const joined = await s.cas(member.key, authority.storeVersion, {
      kind: 'put',
      generationTransition: 'preserve',
      payload: rewriteActorAuthorityRoute(
        authority.payload,
        {
          actorId: 'dependent-member',
          objectGeneration: authority.objectGeneration,
          meshName: 'play',
          nodeRid: authority.allocation.descriptor.rid
        },
        'recovery-spot',
        s.authority.objectGeneration,
        internal.ZLinkSpotKind.User
      )
    });
    assert.equal(joined.kind, 'stored');
    s.lifecycle.confirmAuthority(member.key, { ...joined, kind: 'snapshot' });
    const otherId = 'unrelated-spot';
    const target = {
      meshName: 'play',
      nodeRid: RoutingId.from('recovery-node'),
      nodeLifecycleGeneration: 1n,
      owner: s.originalOwner
    };
    const reserved = await s.store.reserve({
      key: { kind: 'user_spot', globalId: otherId },
      intent: {
        stableType: 'game',
        requestContentReference: 'dependency-test',
        requestSha256: Buffer.alloc(32, 1),
        requestEncodedSize: 1n
      },
      target,
      capacity: { actors: 0, spots: 1 },
      creatingPayload: Buffer.from([1])
    });
    assert.equal(reserved.kind, 'reserved');
    const committed = await s.store.commit({
      key: { kind: 'user_spot', globalId: otherId },
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target,
      readyPayload: encodeServiceUserSpotAuthorityPayload({
        state: 'ready',
        stableType: 'game',
        spotId: otherId,
        ownerId: s.originalOwner.ownerId,
        ownerLeaseGeneration: s.originalOwner.leaseGeneration,
        ownerMeshName: 'play',
        ownerNodeRid: 'recovery-node',
        ownerNodeGeneration: 1n
      })
    });
    assert.equal(committed.kind, 'committed');
    const otherKey = encodeAuthorityKey('user_spot', otherId),
      otherExecution = {};
    s.lifecycle.confirmAuthority(otherKey, committed.ready);
    s.lifecycle.bindSpotExecution('play', otherId, otherExecution);
    s.lifecycle.trackExecution(
      otherExecution,
      () => true,
      () => {}
    );
    if (pendingPhase === 'store') {
      const write = s.backing.write.bind(s.backing);
      s.backing.write = async (request, signal) => {
        if (
          request.mutations.some(
            (item) =>
              item.kind === 'delete' &&
              item.key.value.includes('authority\0actor\0dependent-member')
          )
        )
          await settled.promise;
        return write(request, signal);
      };
    }
    const started = [],
      unrelatedDone = deferred();
    const cleanup = s.store.cleanupDiscardedAuthority.bind(s.store);
    s.store.cleanupDiscardedAuthority = async (key, ...args) => {
      started.push(key.value);
      if (key.value === s.key.value)
        assert.equal((await s.store.readAuthority(member.key)).kind, 'missing');
      const result = await cleanup(key, ...args);
      if (key.value === otherKey.value && result) unrelatedDone.resolve();
      return result;
    };
    s.runtime.discardExecution(s.execution);
    s.runtime.discardExecution(member.execution);
    s.runtime.discardExecution(otherExecution);
    const operation = s.runtime.reclaimOwnerAuthorities();
    await unrelatedDone.promise;
    assert.equal(
      started.includes(s.key.value),
      false,
      'Do not start the dependent Spot qualification or snapshot'
    );
    assert.equal((await s.store.readAuthority(otherKey)).kind, 'missing');
    assert.equal((await s.store.readAuthority(member.key)).kind, 'snapshot');
    settled.resolve();
    await operation;
    await Promise.resolve();
    await s.runtime.reclaimOwnerAuthorities();
    assert.equal((await s.store.readAuthority(member.key)).kind, 'missing');
    assert.equal((await s.store.readAuthority(s.key)).kind, 'missing');
  });

test('shutdown releases the owner and drains a rejected discarded cleanup operation', async (t) => {
  const s = await cleanupFixture(t, true, 'actor');
  const entered = deferred();
  let reject;
  const response = new Promise((_resolve, fail) => {
    reject = fail;
  });
  s.store.cleanupDiscardedAuthority = () => {
    entered.resolve();
    return response;
  };
  s.runtime.discardExecution(s.execution);
  const operation = s.runtime.reclaimOwnerAuthorities();
  const observed = operation.then(
    () => undefined,
    (error) => error
  );
  await entered.promise;
  const stopping = s.runtime.stop();
  reject(new Error('cleanup response failed during shutdown'));
  await assert.rejects(stopping, /Discarded authority cleanup failed during shutdown/);
  assert.equal((await observed).message, 'cleanup response failed during shutdown');
  assert.equal((await s.store.readOwnerLease(s.originalOwner.ownerId)).kind, 'missing');
  assert.equal(s.runtime.ownerLeaseRecoveryRequired, false);
});

for (let iteration = 1; iteration <= 40; iteration++)
  test(`authority Delete capacity race returns Conflict after winner Delete (${iteration})`, async (t) => {
    const s = await cleanupFixture(t, true, 'actor');
    const read = s.backing.read.bind(s.backing);
    const write = s.backing.write.bind(s.backing);
    let race = true;
    let deletes = 0;
    let writes = 0;
    s.backing.write = async (request, signal) => {
      writes++;
      const result = await write(request, signal);
      if (result.kind === 'applied')
        deletes += request.mutations.filter(
          (mutation) => mutation.kind === 'delete' && mutation.key.value.includes('authority\0')
        ).length;
      return result;
    };
    s.backing.read = async (key, signal) => {
      if (race && key.value.startsWith('zlink:v11:capacity:')) {
        race = false;
        assert.equal(
          (await s.cas(s.key, s.authority.storeVersion, { kind: 'delete' })).kind,
          'deleted'
        );
      }
      return read(key, signal);
    };
    let loser;
    try {
      loser = await s.cas(s.key, s.authority.storeVersion, { kind: 'delete' });
    } finally {
      assert.equal(race, false);
      assert.equal(deletes, 1);
      assert.equal(writes, 1);
      assert.equal((await s.store.readAuthority(s.key)).kind, 'missing');
      const capacity = await s.backing.scan({ prefix: 'zlink:v11:capacity:', limit: 10 });
      assert.equal(capacity.value.items.length, 1);
      const usage = JSON.parse(Buffer.from(capacity.value.items[0].value.bytes));
      assert.deepEqual(usage.active, { actors: 0, spots: 0, spotTypes: {} });
    }
    assert.equal(loser.kind, 'conflict');
    assert.equal(loser.current.kind, 'missing');
  });

for (const change of ['unchanged', 'preserve', 'reincarnate'])
  test(`authority Delete capacity underflow checks current incarnation (${change})`, async (t) => {
    const s = await cleanupFixture(t, true, 'instance_spot');
    const read = s.backing.read.bind(s.backing);
    let corrupt = true;
    let latest = s.authority;
    let originalCapacity;
    s.backing.read = async (key, signal) => {
      if (corrupt && key.value.startsWith('zlink:v11:capacity:')) {
        corrupt = false;
        if (change !== 'unchanged') {
          latest = await s.cas(s.key, s.authority.storeVersion, {
            kind: 'put',
            generationTransition: change,
            payload: s.authority.payload
          });
          assert.equal(latest.kind, 'stored');
        }
        const row = await read(key, signal);
        originalCapacity = { key, bytes: row.value.bytes };
        const capacity = JSON.parse(Buffer.from(row.value.bytes));
        capacity.active.spots = 0;
        assert.equal(
          (
            await s.backing.write({
              conditions: [{ kind: 'version', key, expected: row.value.version }],
              mutations: [{ kind: 'put', key, bytes: Buffer.from(JSON.stringify(capacity)) }]
            })
          ).kind,
          'applied'
        );
      }
      return read(key, signal);
    };
    try {
      const deletion = s.cas(s.key, s.authority.storeVersion, { kind: 'delete' });
      if (change === 'unchanged')
        await assert.rejects(deletion, { kind: internal.ZLinkFrameworkErrorKind.DataLost });
      else {
        const result = await deletion;
        assert.equal(result.kind, 'conflict');
        assert.equal(result.current.storeVersion.value, latest.storeVersion.value);
        assert.equal(result.current.objectGeneration, latest.objectGeneration);
        assert.equal(result.current.authorityOwnerGeneration, latest.authorityOwnerGeneration);
      }
    } finally {
      if (originalCapacity !== undefined)
        await s.backing.write({
          conditions: [],
          mutations: [{ kind: 'put', ...originalCapacity }]
        });
    }
    assert.equal((await s.store.readAuthority(s.key)).kind, 'snapshot');
  });
