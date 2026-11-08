'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const internal = require('../../packages/framework/dist/internal');
const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');

async function fixture(t, provider, retention = true) {
  let now = 100;
  let utc = 0;
  let claims = 0;
  let discarded = 0;
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
  const lease = {
    claimOwnerLease: (...args) => {
      claims++;
      return store.claimOwnerLease(...args);
    },
    renewOwnerLease: (...args) => store.renewOwnerLease(...args),
    readOwnerLease: (...args) => store.readOwnerLease(...args),
    releaseOwnerLease: (...args) => store.releaseOwnerLease(...args)
  };
  const runtime = new internal.ZLinkLocationRuntime({
    ownerId: 'reclaim-owner',
    stores: {
      locationStore: store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: lease
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
  await runtime.start({ toString: () => 'reclaim-node' });
  const original = runtime.currentOwnerToken;
  const execution = {};
  runtime.trackExecution(
    execution,
    () => true,
    () => discarded++
  );
  t.after(async () => {
    await runtime.stop();
    assert.equal(timers.size, 0);
  });
  return {
    runtime,
    store,
    lease,
    original,
    execution,
    claims: () => claims - 1,
    discarded: () => discarded,
    episode: () => runtime[inspectRuntime]().storeOutage,
    setNow(value) {
      now = value;
    },
    expire() {
      utc = 1000;
    },
    async outage() {
      now = 200;
      lease.renewOwnerLease = async () => {
        throw new Error('Store unavailable');
      };
      assert.equal(await runtime.renewOwnerLeaseOnce(), false);
      lease.renewOwnerLease = (...args) => store.renewOwnerLease(...args);
    }
  };
}

for (const provider of [false, true]) {
  for (const retention of [false, true]) {
    for (const boundary of [1199, 1200, 1201]) {
      test(`D17 Missing at lifetime+margin ${boundary - 1200} (${provider}/${retention}), 100 iterations`, async (t) => {
        for (let iteration = 0; iteration < 100; iteration++) {
          await t.test(String(iteration), async (t) => {
            const s = await fixture(t, provider, retention);
            await s.outage();
            const episode = s.episode();
            await s.store.releaseOwnerLease(s.original);
            s.setNow(boundary);
            const eligible = boundary >= 1200;
            assert.equal(await s.runtime.renewOwnerLeaseOnce(), eligible);
            assert.equal(s.claims(), eligible ? 1 : 0);
            if (eligible) assert.notDeepEqual(s.runtime.currentOwnerToken, s.original);
            else assert.deepEqual(s.runtime.currentOwnerToken, s.original);
            assert.equal(s.episode().startedAtMs, episode.startedAtMs);
            assert.equal(s.episode().cutoffMs, episode.cutoffMs);
            assert.equal(s.episode().cutoffMs, retention ? 1300 : 1000);
          });
        }
      });
    }
  }
  for (const state of ['expired', 'live', 'different-token', 'read-error']) {
    test(`D17 exact read ${state} (${provider}), 100 iterations`, async (t) => {
      for (let iteration = 0; iteration < 100; iteration++) {
        await t.test(String(iteration), async (t) => {
          const s = await fixture(t, provider);
          await s.outage();
          const episode = s.episode();
          s.setNow(1201);
          s.lease.renewOwnerLease = async () => ({ kind: 'stale' });
          if (state === 'expired') {
            s.expire();
            s.lease.readOwnerLease = async () => ({
              kind: 'found',
              token: s.original,
              leaseExpiresAt: new Date(1000),
              storeNow: new Date(1000)
            });
          } else if (state === 'different-token') {
            await s.store.releaseOwnerLease(s.original);
            assert.equal((await s.store.claimOwnerLease(s.original.ownerId, 1000)).kind, 'claimed');
          } else if (state === 'read-error') {
            s.lease.readOwnerLease = async () => {
              throw new Error('exact read unavailable');
            };
          }
          assert.equal(await s.runtime.renewOwnerLeaseOnce(), state === 'expired');
          assert.equal(s.claims(), state === 'expired' ? 1 : 0);
          if (state === 'expired') assert.notDeepEqual(s.runtime.currentOwnerToken, s.original);
          else assert.deepEqual(s.runtime.currentOwnerToken, s.original);
          assert.equal(s.episode().startedAtMs, episode.startedAtMs);
          assert.equal(s.episode().cutoffMs, 1300);
          assert.equal(s.episode().discarded, state === 'different-token');
          assert.equal(s.discarded(), state === 'different-token' ? 1 : 0);
          if (state === 'different-token') {
            assert.throws(() => s.runtime.executionAllowed(s.execution), {
              kind: internal.ZLinkFrameworkErrorKind.NotFound
            });
            assert.equal(await s.runtime.renewOwnerLeaseOnce(), false);
            assert.equal(s.claims(), 0);
            assert.equal(s.discarded(), 1);
          }
        });
      }
    });
  }
  test(`D17 Missing outside an outage reclaims immediately (${provider}), 100 iterations`, async (t) => {
    for (let iteration = 0; iteration < 100; iteration++) {
      await t.test(String(iteration), async (t) => {
        const s = await fixture(t, provider);
        await s.store.releaseOwnerLease(s.original);
        s.setNow(200);
        assert.equal(await s.runtime.renewOwnerLeaseOnce(), true);
        assert.equal(s.claims(), 1);
        assert.notDeepEqual(s.runtime.currentOwnerToken, s.original);
        assert.equal(s.episode(), undefined);
      });
    }
  });
}
