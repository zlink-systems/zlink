'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { RoutingId } = require('@zlink-systems/zlink');
const internal = require('../../packages/framework/dist/internal');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  publishInitialActorAuthority,
  isActorAuthorityPayload,
  rewriteActorAuthorityOwner
} = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const {
  encodeServiceUserSpotAuthorityPayload,
  encodeServiceInstanceAuthorityPayload,
  rewriteServiceAuthorityOwner
} = require('../../packages/framework/dist/runtime/foundation/service-authority-payload-codec');

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}
async function fixture(t, provider, kind = 'actor', capacity = 10, onDiscard) {
  let now = 100,
    utc = 0;
  const timers = new Set();
  const date = () => new Date(utc);
  const store = provider
    ? new internal.ZLinkLocationStoreRepository(
        new internal.ZLinkInMemoryProviderLocationStore(date),
        date,
        undefined,
        () => now,
        0
      )
    : new internal.ZLinkInMemoryLocationStore(date, () => now, 0);
  const lease = Object.fromEntries(
    ['claimOwnerLease', 'renewOwnerLease', 'readOwnerLease', 'releaseOwnerLease'].map((name) => [
      name,
      (...args) => store[name](...args)
    ])
  );
  const runtime = new internal.ZLinkLocationRuntime({
    ownerId: 'recovery-owner',
    rewriteAuthorityPayloadForOwner: (payload, owner) =>
      isActorAuthorityPayload(payload)
        ? rewriteActorAuthorityOwner(payload, owner)
        : (rewriteServiceAuthorityOwner(payload, owner) ?? payload),
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
      storeOutageRetention: true,
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
      onDiscard?.(error);
    }
  );
  const writes = [];
  const legacyWrites = [];
  for (const name of ['updateActor', 'updateSpot']) {
    const update = store[name].bind(store);
    store[name] = async (...args) => {
      legacyWrites.push({ name, args });
      return update(...args);
    };
  }
  const cas = store.compareExchangeAuthority.bind(store);
  store.compareExchangeAuthority = async (...args) => {
    writes.push(args);
    return cas(...args);
  };
  t.after(async () => {
    lifecycle.dispose();
    await runtime.stop();
    assert.equal(timers.size, 0);
  });
  return {
    store,
    descriptor,
    runtime,
    lifecycle,
    execution,
    key,
    authority,
    originalOwner,
    writes,
    legacyWrites,
    cas,
    discarded: () => discarded,
    async addActor(actorId, objectGeneration = 2n) {
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
        }
      );
      return { key: actorKey, execution: owner, discarded: () => disposed };
    },
    setNow(value) {
      now = value;
    },
    async outage(at = 200) {
      legacyWrites.length = 0;
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
    publishRuntime(state = internal.ZLinkFrameworkRuntimeState.Preparing) {
      return runtime.writeMeshNode(
        { ...descriptor(runtime.currentOwnerToken), state },
        internal.ZLinkLocationWriteIntent.Renew
      );
    },
    publish() {
      return store
        .updateMeshNode(
          descriptor(runtime.currentOwnerToken),
          internal.ZLinkInternalMeshNodeWriteIntent.RecoveryClaim
        )
        .then((result) => {
          assert.equal(result.status, 'stored');
        });
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
    },
    lease
  };
}

for (const provider of [false, true]) {
  for (const kind of ['actor', 'user_spot', 'instance_spot']) {
    test(`Preserve renew keeps token and waits for both confirmations (${provider}/${kind})`, async (t) => {
      const s = await fixture(t, provider, kind);
      await s.outage();
      assert.equal(await s.renew(), true);
      assert.equal(s.runtime.currentOwnerToken.leaseGeneration, s.originalOwner.leaseGeneration);
      assert.equal(s.runtime.executionAllowed(s.execution), false);
      const publication = deferred();
      const operation = s.lifecycle.reclaimOwnerRows(publication.promise);
      assert.equal(s.runtime.executionAllowed(s.execution), false);
      publication.resolve();
      await operation;
      assert.equal(s.runtime.executionAllowed(s.execution), true);
      assert.equal(s.writes.length, 0);
      assert.equal(s.discarded(), 0);
    });
    test(`Preserve submits exact CAS before descriptor reply and excludes reclaim (${provider}/${kind})`, async (t) => {
      const s = await fixture(t, provider, kind);
      const identity = s.execution.binding.identity;
      await s.outage();
      assert.equal(await s.renew(true), true);
      await s.publish(); // Commit is visible while the descriptor reply is held.
      const publication = deferred(),
        submitted = deferred();
      s.store.compareExchangeAuthority = async (...args) => {
        s.writes.push(args);
        const result = await s.cas(...args);
        submitted.resolve();
        return result;
      };
      const operation = s.lifecycle.reclaimOwnerRows(publication.promise);
      await submitted.promise;
      assert.equal(s.runtime.executionAllowed(s.execution), false);
      assert.equal(s.writes.length, 1);
      assert.equal(s.writes[0][1].value, s.authority.storeVersion.value);
      assert.equal(s.writes[0][2].kind, 'rebindOwnerLease');
      publication.resolve();
      await operation;
      assert.equal(s.writes.length, 1);
      assert.deepEqual(s.legacyWrites, []);
      const current = await s.store.readAuthority(s.key);
      assert.equal(current.objectGeneration, s.authority.objectGeneration);
      assert.equal(current.authorityOwnerGeneration, s.authority.authorityOwnerGeneration);
      assert.deepEqual(current.allocation, s.authority.allocation);
      assert.equal(s.runtime.executionAllowed(s.execution), true);
      assert.equal(s.execution.binding.identity, identity);
      assert.equal(s.execution.binding.sequence, 7n);
      assert.equal(s.discarded(), 0);
    });
  }
  for (const offset of [0, 1]) {
    test(`Preserve submission timeout boundary T${offset ? '+1' : ''} (${provider})`, async (t) => {
      const s = await fixture(t, provider);
      await s.outage();
      await s.renew(true);
      await s.publish();
      s.setNow(1250 + offset);
      await s.lifecycle.reclaimOwnerRows();
      assert.equal(s.writes.length, offset ? 0 : 1);
      if (offset)
        assert.throws(() => s.runtime.executionAllowed(s.execution), {
          kind: internal.ZLinkFrameworkErrorKind.NotFound
        });
      else assert.equal(s.runtime.executionAllowed(s.execution), true);
      assert.equal(s.discarded(), offset);
    });
  }
  for (const damage of ['version', 'missing', 'owner', 'incarnation', 'lifecycle']) {
    test(`Preserve rejects ${damage} and subsequent reclaim cannot revive it (${provider})`, async (t) => {
      const s = await fixture(t, provider);
      await s.outage();
      await s.renew(true);
      await s.publish();
      const read = s.store.readAuthority.bind(s.store);
      s.store.readAuthority = async (...args) => {
        const current = await read(...args);
        if (args[0].value !== s.key.value || current.kind !== 'snapshot') return current;
        if (damage === 'missing') return { kind: 'missing', storeNow: current.storeNow };
        if (damage === 'version')
          return { ...current, storeVersion: { value: 'different-version' } };
        if (damage === 'owner') return { ...current, ownerId: 'another-owner' };
        if (damage === 'incarnation')
          return { ...current, objectGeneration: current.objectGeneration + 1n };
        return {
          ...current,
          allocation: { ...current.allocation, descriptorLifecycleGeneration: 2n }
        };
      };
      await s.lifecycle.reclaimOwnerRows();
      await s.lifecycle.reclaimOwnerRows();
      assert.equal(s.writes.length, 0);
      assert.deepEqual(s.legacyWrites, []);
      assert.equal(s.discarded(), 1);
      assert.throws(() => s.runtime.executionAllowed(s.execution), {
        kind: internal.ZLinkFrameworkErrorKind.NotFound
      });
      assert.equal((await read(s.key)).storeVersion.value, s.authority.storeVersion.value);
    });
  }
  test(`Preserve cannot resume a CAS reply at T (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    await s.publish();
    const reply = deferred(),
      submitted = deferred();
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      await reply.promise;
      return result;
    };
    const operation = s.lifecycle.reclaimOwnerRows();
    await submitted.promise;
    assert.equal(s.runtime.executionAllowed(s.execution), false);
    s.setNow(1300);
    reply.resolve();
    await operation;
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.discarded(), 1);
    assert.equal(s.writes.length, 1);
  });
}

for (const provider of [false, true]) {
  test(`Preserve descriptor alone cannot open a pending CAS (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    const publication = s.publish();
    const reply = deferred(),
      submitted = deferred();
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      await reply.promise;
      return result;
    };
    const operation = s.lifecycle.reclaimOwnerRows(publication);
    await submitted.promise;
    await publication;
    assert.equal(s.runtime.executionAllowed(s.execution), false);
    reply.resolve();
    await operation;
    assert.equal(s.runtime.executionAllowed(s.execution), true);
    assert.equal(s.writes.length, 1);
  });
  test(`Preserve replaced owner token is terminal without another claim (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    s.expire();
    assert.equal((await s.store.claimOwnerLease(s.originalOwner.ownerId, 1000)).kind, 'claimed');
    let claims = 0;
    s.lease.claimOwnerLease = async (...args) => {
      claims++;
      return s.store.claimOwnerLease(...args);
    };
    assert.equal(await s.renew(), false);
    assert.equal(claims, 0);
    assert.equal(s.runtime.ownerLeaseUsable, false);
    assert.equal(s.discarded(), 1);
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
  });
  test(`Preserve definite descriptor rejection discards the object (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    await s.publish();
    const {
      ZLinkMeshNodeDescriptorConflictException
    } = require('../../packages/framework/dist/runtime/spots/spot-node-runtime-manager');
    await assert.rejects(
      s.lifecycle.reclaimOwnerRows(
        Promise.reject(new ZLinkMeshNodeDescriptorConflictException('descriptor rejected'))
      ),
      /descriptor rejected/
    );
    assert.equal(s.discarded(), 1);
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
  });
  test(`Preserve heartbeat reuses a confirmed CAS after an unconfirmed publication (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    await s.publish();
    const publication = deferred(),
      submitted = deferred();
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      return result;
    };
    const first = s.lifecycle.reclaimOwnerRows(publication.promise);
    await submitted.promise;
    publication.reject(new Error('publication response unavailable'));
    await assert.rejects(first, /publication response unavailable/);
    assert.equal(s.runtime.executionAllowed(s.execution), false);
    await s.lifecycle.reclaimOwnerRows();
    assert.equal(s.writes.length, 1);
    assert.equal(s.runtime.executionAllowed(s.execution), true);
  });
}

for (const provider of [false, true]) {
  test(`Preserve actual descriptor submission starts CAS before its reply (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    const reply = deferred(),
      submitted = deferred(),
      descriptorCommitted = deferred();
    t.after(() => reply.resolve());
    const update = s.store.updateMeshNode.bind(s.store);
    s.store.updateMeshNode = async (...args) => {
      const result = await update(...args);
      if (result.status === 'stored') {
        descriptorCommitted.resolve();
        await reply.promise;
      }
      return result;
    };
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      return result;
    };
    const publication = s.publishRuntime();
    await Promise.all([submitted.promise, descriptorCommitted.promise]);
    assert.equal(s.writes.length, 1);
    assert.equal(s.runtime.executionAllowed(s.execution), false);
    reply.resolve();
    await publication;
    await s.lifecycle.reclaimOwnerRows();
    assert.equal(s.runtime.executionAllowed(s.execution), true);
    assert.equal(s.writes.length, 1);
  });
  test(`Preserve descriptor confirmation at T cannot reopen execution (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    await s.publish();
    const publication = deferred(),
      submitted = deferred();
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      return result;
    };
    const operation = s.lifecycle.reclaimOwnerRows(publication.promise);
    await submitted.promise;
    s.setNow(1300);
    publication.resolve();
    await operation;
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.discarded(), 1);
  });
}

for (const provider of [false, true]) {
  for (const failure of ['invalid-new-lease', 'foreign-owner', 'new-lifecycle']) {
    test(`Preserve Store rejects ${failure} with the same retained fence (${provider})`, async (t) => {
      const s = await fixture(t, provider);
      await s.outage();
      await s.renew(true);
      const owner = s.runtime.currentOwnerToken;
      let targetOwner = owner;
      if (failure === 'invalid-new-lease') await s.store.releaseOwnerLease(owner);
      else if (failure === 'new-lifecycle') {
        assert.equal(
          (
            await s.store.updateMeshNode(
              { ...s.descriptor(owner), lifecycleGeneration: 2n },
              internal.ZLinkInternalMeshNodeWriteIntent.RecoveryClaim
            )
          ).status,
          'stored'
        );
      } else {
        const foreign = await s.store.claimOwnerLease('foreign-owner', 1000);
        assert.equal(foreign.kind, 'claimed');
        targetOwner = foreign.token;
      }
      const {
        rewriteActorAuthorityOwner
      } = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
      const mutation = {
        kind: 'rebindOwnerLease',
        expectedOwner: s.originalOwner,
        targetOwner,
        payload: rewriteActorAuthorityOwner(s.authority.payload, targetOwner)
      };
      if (failure === 'foreign-owner')
        await assert.rejects(s.cas(s.key, s.authority.storeVersion, mutation), TypeError);
      else assert.equal((await s.cas(s.key, s.authority.storeVersion, mutation)).kind, 'conflict');
      assert.equal(
        (await s.store.readAuthority(s.key)).storeVersion.value,
        s.authority.storeVersion.value
      );
    });
  }
}

for (const provider of [false, true]) {
  test(`Preserve completion keeps the last successful lease start after a failed heartbeat (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew();
    const publication = deferred();
    const recovery = s.lifecycle.reclaimOwnerRows(publication.promise);
    s.setNow(500);
    s.lease.renewOwnerLease = async () => {
      throw new Error('heartbeat unavailable');
    };
    assert.equal(await s.runtime.renewOwnerLeaseOnce(), false);
    publication.resolve();
    await recovery;
    const {
      inspectRuntime
    } = require('../../packages/framework/dist/runtime/internal-observation');
    assert.equal(s.runtime[inspectRuntime]().lastSuccessfulLeaseStartedAtMs, 300);
  });
}

for (const provider of [false, true]) {
  test(`Preserve cannot resume a CAS confirmed for a superseded local lease (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    await s.publish();
    const reply = deferred(),
      submitted = deferred();
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      await reply.promise;
      return result;
    };
    const operation = s.lifecycle.reclaimOwnerRows();
    await submitted.promise;
    const owner = s.runtime.currentOwnerToken;
    s.setNow(2300);
    s.expire();
    assert.equal(await s.runtime.renewOwnerLeaseOnce(), true);
    assert.notEqual(s.runtime.currentOwnerToken.leaseGeneration, owner.leaseGeneration);
    reply.resolve();
    await operation;
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.discarded(), 1);
    assert.equal(s.writes.length, 1);
  });
}

for (const provider of [false, true]) {
  test(`Preserve heartbeat confirms an indeterminate CAS without resubmission (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    await s.publish();
    const read = s.store.readAuthority.bind(s.store);
    let reads = 0;
    s.store.readAuthority = async (...args) => {
      if (args[0].value === s.key.value && ++reads === 2)
        throw new Error('confirmation unavailable');
      return read(...args);
    };
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      await s.cas(...args);
      throw new Error('CAS reply unavailable');
    };
    await assert.rejects(s.lifecycle.reclaimOwnerRows(), /confirmation unavailable/);
    assert.equal(s.runtime.executionAllowed(s.execution), false);
    await s.lifecycle.reclaimOwnerRows();
    assert.equal(s.runtime.executionAllowed(s.execution), true);
    assert.equal(s.discarded(), 0);
    assert.equal(s.writes.length, 1);
  });
}

for (const provider of [false, true]) {
  test(`Preserve resumes each object independently and keeps the pending object's original T (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    const other = await s.addActor('second-recovery-actor');
    await s.outage();
    await s.renew(true);
    await s.publish();
    const reply = deferred(),
      pendingSubmitted = deferred(),
      otherSubmitted = deferred();
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      if (args[0].value === s.key.value) {
        pendingSubmitted.resolve();
        await reply.promise;
      } else otherSubmitted.resolve();
      return result;
    };
    const operation = s.lifecycle.reclaimOwnerRows();
    await Promise.all([pendingSubmitted.promise, otherSubmitted.promise]);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(s.runtime.executionAllowed(s.execution), false);
    assert.equal(s.runtime.executionAllowed(other.execution), true);
    s.setNow(1200);
    assert.equal(await s.runtime.renewOwnerLeaseOnce(), true);
    s.tick(1300);
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.runtime.executionAllowed(other.execution), true);
    assert.equal(s.discarded(), 1);
    assert.equal(other.discarded(), 0);
    reply.resolve();
    await operation;
    assert.equal(s.writes.length, 2);
    assert.deepEqual(s.legacyWrites, []);
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.runtime.executionAllowed(other.execution), true);
  });
}

for (const provider of [false, true]) {
  test(`Preserve checks lifecycle and the current lease while a descriptor for an intermediate lease is visible (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.renew(true, 300);
    await s.publish();
    await s.outage(400);
    s.expire();
    assert.equal(await s.renew(false, 1400), true);
    const owner = s.runtime.currentOwnerToken;
    const descriptorReply = deferred(),
      submitted = deferred();
    const update = s.store.updateMeshNode.bind(s.store);
    s.store.updateMeshNode = async (...args) => {
      if (args[1] === internal.ZLinkInternalMeshNodeWriteIntent.RecoveryClaim)
        await descriptorReply.promise;
      return update(...args);
    };
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      return result;
    };
    const publication = s.publishRuntime();
    await submitted.promise;
    assert.equal(s.runtime.executionAllowed(s.execution), false);
    descriptorReply.resolve();
    await publication;
    await s.lifecycle.reclaimOwnerRows();
    assert.equal((await s.store.readAuthority(s.key)).ownerLeaseGeneration, owner.leaseGeneration);
    assert.equal(s.runtime.executionAllowed(s.execution), true);
    assert.equal(s.writes.length, 1);
    assert.deepEqual(s.legacyWrites, []);
  });
}

for (const provider of [false, true]) {
  test(`Preserve late CAS confirmation does not overwrite a replacement incarnation's fence (${provider})`, async (t) => {
    const s = await fixture(t, provider);
    await s.outage();
    await s.renew(true);
    await s.publish();
    const reply = deferred(),
      submitted = deferred();
    s.store.compareExchangeAuthority = async (...args) => {
      s.writes.push(args);
      const result = await s.cas(...args);
      submitted.resolve();
      await reply.promise;
      return result;
    };
    const operation = s.lifecycle.reclaimOwnerRows();
    await submitted.promise;
    s.runtime.discardExecution(s.execution);
    await s.lifecycle.releaseActor('player', 'recovery-actor');
    const row = await s.store.resolveActor({ meshName: 'play', actorId: 'recovery-actor' });
    if (row !== undefined) {
      assert.equal(
        (await s.runtime.writeActor(row, internal.ZLinkLocationWriteIntent.Takeover)).status,
        'stored'
      );
      assert.equal(
        await s.runtime.removeActor(
          { meshName: 'play', actorId: 'recovery-actor' },
          s.runtime.currentOwnerToken.leaseGeneration
        ),
        'stored'
      );
    }
    const prior = await s.store.readAuthority(s.key);
    if (prior.kind === 'snapshot')
      assert.equal((await s.cas(s.key, prior.storeVersion, { kind: 'delete' })).kind, 'deleted');
    let factory;
    const track = s.runtime.trackExecution.bind(s.runtime);
    s.runtime.trackExecution = (owner, ready, discard, capture) => {
      factory = capture;
      return track(owner, ready, discard, capture);
    };
    const replacement = await s.addActor('recovery-actor');
    const current = await s.store.readAuthority(s.key);
    assert.equal(current.kind, 'snapshot');
    assert.notEqual(current.objectGeneration, s.authority.objectGeneration);
    s.legacyWrites.length = 0;
    reply.resolve();
    await operation;
    assert.equal(typeof factory, 'function');
    const proof = factory(new WeakRef(replacement.execution));
    assert.equal(proof.incarnation.objectGeneration, current.objectGeneration);
    assert.equal(proof.incarnation.authorityOwnerGeneration, current.authorityOwnerGeneration);
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.runtime.executionAllowed(replacement.execution), true);
    assert.deepEqual(s.legacyWrites, []);
  });
}

for (const provider of [false, true]) {
  for (const kind of ['actor', 'user_spot', 'instance_spot']) {
    test(`Preserve uses the confirmed fence after a claim without an outage (${provider}/${kind})`, async (t) => {
      const s = await fixture(t, provider, kind);
      assert.equal(await s.renew(true, 1100), true);
      await s.publish();
      await s.lifecycle.reclaimOwnerRows();
      const current = await s.store.readAuthority(s.key);
      assert.equal(current.ownerLeaseGeneration, s.runtime.currentOwnerToken.leaseGeneration);
      const writes = s.writes.length;
      await s.outage(1200);
      assert.equal(await s.renew(false, 1300), true);
      await s.lifecycle.reclaimOwnerRows();
      assert.equal(s.runtime.executionAllowed(s.execution), true);
      assert.equal(s.discarded(), 0);
      assert.equal(s.writes.length, writes);
      assert.deepEqual(s.legacyWrites, []);
    });
  }
}

async function addScaleSpot(s, index, kind) {
  const id = `scale-spot-${index}`;
  const owner = s.runtime.currentOwnerToken;
  const rid = s.descriptor(owner).rid;
  if (kind === 'user_spot')
    assert.equal(
      await s.lifecycle.claimSpot('play', id, 'game', rid, internal.ZLinkSpotKind.User, 1n, 1n),
      'stored'
    );
  const target = { meshName: 'play', nodeRid: rid, nodeLifecycleGeneration: 1n, owner };
  const reserved = await s.store.reserve({
    key: { kind, globalId: id },
    intent: {
      stableType: 'game',
      requestContentReference: 'scale-test',
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 1n
    },
    target,
    capacity: { actors: 0, spots: 1 },
    creatingPayload: Buffer.from([1])
  });
  assert.equal(reserved.kind, 'reserved');
  const committed = await s.store.commit({
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
      ownerId: owner.ownerId,
      ownerLeaseGeneration: owner.leaseGeneration,
      ownerMeshName: 'play',
      ownerNodeRid: String(rid),
      ownerNodeGeneration: 1n
    })
  });
  assert.equal(committed.kind, 'committed');
  const key = encodeAuthorityKey(kind, id);
  if (kind === 'instance_spot')
    s.lifecycle.trackInstanceSpot({
      meshName: 'play',
      spotId: id,
      stableType: 'game',
      nodeRid: rid,
      nodeGeneration: 1n,
      objectGeneration: committed.ready.objectGeneration,
      authorityOwnerGeneration: committed.ready.authorityOwnerGeneration,
      ownerId: owner.ownerId,
      ownerLeaseGeneration: owner.leaseGeneration,
      storeVersion: committed.ready.storeVersion.value
    });
  s.lifecycle.confirmAuthority(key, committed.ready);
  const execution = {};
  s.lifecycle.bindSpotExecution('play', id, execution);
  s.lifecycle.trackExecution(
    execution,
    () => true,
    () => {}
  );
  return { key, execution };
}

function iterationProbe(t) {
  const counts = { claims: 0, retained: 0 };
  function count(value) {
    if (value?.executionOwner instanceof WeakRef) counts.claims++;
    if (value?.phase !== undefined && Object.hasOwn(value, 'recover')) counts.retained++;
  }
  for (const method of ['values', Symbol.iterator]) {
    const original = Map.prototype[method];
    Map.prototype[method] = function* () {
      for (const value of original.call(this)) {
        count(method === 'values' ? value : value[1]);
        yield value;
      }
    };
    t.after(() => {
      Map.prototype[method] = original;
    });
  }
  return counts;
}

for (const provider of [false, true]) {
  test(`5000 retained executions use bounded admission, capture and reclaim lookups (${provider})`, async (t) => {
    const s = await fixture(t, provider, 'actor', 10000);
    const rows = [{ key: s.key, execution: s.execution }];
    for (let index = 1; index < 5000; index++)
      rows.push(
        index % 3 === 0
          ? await s.addActor(`scale-actor-${index}`, BigInt(index + 1))
          : await addScaleSpot(s, index, index % 3 === 1 ? 'user_spot' : 'instance_spot')
      );
    const counts = iterationProbe(t);
    let derefs = 0;
    const deref = WeakRef.prototype.deref;
    WeakRef.prototype.deref = function () {
      derefs++;
      return deref.call(this);
    };
    t.after(() => {
      WeakRef.prototype.deref = deref;
    });
    await s.outage();
    const captureDerefs = derefs;
    const captureVisits = counts.claims;

    s.setNow(1000);
    derefs = 0;
    const retainedBeforeAdmission = counts.retained;
    await t.test('admission has no retained dereferences or iterations', () => {
      assert.equal(s.runtime.executionAllowed(rows.at(-1).execution), true);
      assert.equal(s.runtime.executionAllowed({}), false);
      assert.equal(derefs, 0, 'admission must not dereference any retained execution');
      assert.equal(counts.retained, retainedBeforeAdmission, 'admission must not iterate records');
    });
    const admissionDerefs = derefs;
    assert.equal(s.runtime.ownerLeaseRecoveryRequired, true);

    // A new lease forces captureRecovery's post-CAS currentTracked lookup as well.
    assert.equal(await s.renew(true, 1200), true);
    counts.claims = 0;
    const excluded = await s.runtime.reclaimOwnerAuthorities(s.publish());
    await t.test(
      'capture and post-CAS validation visit tracked items a bounded number of times',
      () => {
        assert.ok(captureDerefs <= rows.length * 3, `capture derefs=${captureDerefs}`);
        assert.ok(captureVisits <= rows.length * 2, `capture tracked visits=${captureVisits}`);
        assert.ok(counts.claims <= rows.length * 2, `post-CAS tracked visits=${counts.claims}`);
      }
    );
    for (const row of rows) assert.equal(s.runtime.executionAllowed(row.execution), true);
    assert.equal(s.runtime.ownerLeaseRecoveryRequired, false);

    await t.test('reclaim exclusion does not visit retained records per row', async () => {
      counts.retained = 0;
      for (const row of rows) {
        const authority = await s.store.readAuthority(row.key);
        assert.equal(excluded(row.key, authority), true);
        assert.equal(counts.retained, 0, 'row exclusion must not iterate retained records');
        assert.equal(excluded(row.key), false);
        assert.equal(excluded(row.key, { ...authority, objectGeneration: 999999n }), false);
        assert.equal(excluded(row.key, { ...authority, authorityOwnerGeneration: 999999n }), false);
      }
      assert.equal(excluded(encodeAuthorityKey('actor', 'not-retained'), s.authority), false);
      assert.equal(counts.retained, 0, 'row exclusion must not iterate retained records');
    });
    t.diagnostic(
      `objects=5000 captureDerefs=${captureDerefs} captureVisits=${captureVisits} admissionDerefs=${admissionDerefs} postCasVisits=${counts.claims} exclusionVisits=${counts.retained}`
    );
  });
}

for (const kind of ['actor', 'user_spot', 'instance_spot']) {
  test(`execution lookup follows rebind (${kind})`, async (t) => {
    const s = await fixture(t, false, kind);
    const oldExecution = s.execution;
    const replacement = {};
    if (kind === 'actor') s.lifecycle.bindActorExecution('recovery-actor', replacement);
    else s.lifecycle.bindSpotExecution('play', 'recovery-spot', replacement);
    let discarded = false;
    s.lifecycle.trackExecution(
      replacement,
      () => true,
      () => {
        discarded = true;
      }
    );
    await s.outage();
    assert.equal(await s.renew(), true);
    await s.lifecycle.reclaimOwnerRows(s.publish());
    assert.throws(() => s.runtime.executionAllowed(oldExecution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.runtime.executionAllowed(replacement), true);
    assert.equal(discarded, false);
  });

  test(`execution lookup forgets released rows (${kind})`, async (t) => {
    const s = await fixture(t, false, kind);
    if (kind === 'actor') await s.lifecycle.releaseActor('player', 'recovery-actor');
    else await s.lifecycle.releaseSpot('play', 'recovery-spot');
    await s.outage();
    assert.equal(await s.renew(), true);
    await s.lifecycle.reclaimOwnerRows(s.publish());
    assert.throws(() => s.runtime.executionAllowed(s.execution), {
      kind: internal.ZLinkFrameworkErrorKind.NotFound
    });
    assert.equal(s.discarded(), 1);
  });
}

test('reclaim exclusion indexes every retained incarnation sharing an authority key', async (t) => {
  const s = await fixture(t, false);
  const key = encodeAuthorityKey('actor', 'shared-key');
  const incarnations = [
    { objectGeneration: 10n, authorityOwnerGeneration: 20n },
    { objectGeneration: 10n, authorityOwnerGeneration: 21n },
    { objectGeneration: 11n, authorityOwnerGeneration: 22n }
  ];
  for (const incarnation of incarnations)
    s.runtime.trackExecution(
      {},
      () => true,
      () => {},
      () => ({
        key,
        meshName: 'play',
        incarnation,
        run: async () => true
      })
    );
  await s.outage();
  assert.equal(await s.renew(), true);
  const excluded = await s.runtime.reclaimOwnerAuthorities(s.publish());
  for (const incarnation of incarnations) assert.equal(excluded(key, incarnation), true);
  assert.equal(excluded(key, { objectGeneration: 10n, authorityOwnerGeneration: 22n }), false);
  assert.equal(excluded(key, { objectGeneration: 11n, authorityOwnerGeneration: 20n }), false);
  assert.equal(excluded(key), false);
});

for (const provider of [false, true]) {
  for (const loss of ['replaced-token', 'preserve-rejected']) {
    test(`queued ownership loss is NotFound (${provider}/${loss}) repeated 100 times`, async (t) => {
      for (let iteration = 0; iteration < 100; iteration++) {
        await t.test(String(iteration), async (t) => {
          const s = await fixture(t, provider, 'actor', 10, (error) =>
            serial.discardPendingApplication(error)
          );
          const serial = new internal.ZLinkSerialExecutionQueue(async (record) => {
            try {
              record.resolve(await record.operation());
            } catch (error) {
              record.reject(error);
            } finally {
              record.release();
            }
          });
          serial.setExecutionPolicy(() => () => s.runtime.executionAllowed(s.execution));
          let finish, signalStarted;
          const started = new Promise((resolve) => {
            signalStarted = resolve;
          });
          const running = serial.submit(
            () =>
              new Promise((resolve) => {
                finish = resolve;
                signalStarted();
              })
          );
          await started;
          const work = serial.submit(() => assert.fail('application must not run'));
          const continuation = serial.submitContinuation(() =>
            assert.fail('continuation must not run')
          );
          const request = serial.submit(() => assert.fail('request must not run'));
          const outcomes = Promise.allSettled([work, continuation, request]);
          try {
            await s.outage();
            if (loss === 'replaced-token') {
              s.expire();
              assert.equal(
                (await s.store.claimOwnerLease(s.originalOwner.ownerId, 1000)).kind,
                'claimed'
              );
              assert.equal(await s.renew(), false);
            } else {
              await s.renew(true);
              await s.publish();
              s.store.compareExchangeAuthority = async () => ({
                kind: 'conflict',
                storeNow: new Date(0)
              });
              await s.lifecycle.reclaimOwnerRows();
            }
          } finally {
            finish();
          }
          await running;
          for (const result of await outcomes) {
            assert.equal(result.status, 'rejected');
            assert.equal(result.reason?.kind, internal.ZLinkFrameworkErrorKind.NotFound);
          }
          assert.equal(s.discarded(), 1);
        });
      }
    });
  }
}
