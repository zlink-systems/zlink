'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const internal = require('../../packages/framework/dist/internal');
const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const {
  zlinkDefaultLocationOptions
} = require('../../packages/framework/dist/contracts/Locations/Options');

async function scenario(t, provider, retention = false) {
  let now = 100;
  const timers = new Set();
  const store = provider
    ? new internal.ZLinkLocationStoreRepository(
        new internal.ZLinkInMemoryProviderLocationStore(() => new Date(0)),
        () => new Date(0),
        undefined,
        () => now,
        0
      )
    : new internal.ZLinkInMemoryLocationStore(
        () => new Date(0),
        () => now,
        0
      );
  const lease = {
    claimOwnerLease: (...args) => store.claimOwnerLease(...args),
    renewOwnerLease: (...args) => store.renewOwnerLease(...args),
    readOwnerLease: (...args) => store.readOwnerLease(...args),
    releaseOwnerLease: (...args) => store.releaseOwnerLease(...args)
  };
  const options = {
    storeOutageRetention: retention,
    ownerLeaseTtlMs: 1000,
    ownerLeaseFencingMarginMs: 100
  };
  const runtime = new internal.ZLinkLocationRuntime({
    stores: {
      locationStore: store,
      authorityStore: store,
      peerStore: store,
      spotStore: store,
      actorStore: store,
      routeStore: store,
      ownerLeaseStore: lease
    },
    ownerId: 'owner',
    options,
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
  await runtime.start({ toString: () => 'node' });
  t.after(async () => {
    await runtime.stop();
    assert.equal(timers.size, 0);
  });
  return {
    runtime,
    lease,
    options,
    store,
    snapshot: () => runtime[inspectRuntime](),
    advance(value) {
      now = value;
      for (const timer of [...timers])
        if (timer.at <= now) {
          timers.delete(timer);
          timer.callback();
        }
    },
    setNow(value) {
      now = value;
    },
    fail() {
      lease.renewOwnerLease = async () => {
        throw new Error('provider unavailable');
      };
    }
  };
}

for (const provider of [false, true]) {
  for (const retention of [false, true]) {
    test(`outage cutoff and scheduled irreversible latch (provider=${provider}, retention=${retention})`, async (t) => {
      const s = await scenario(t, provider, retention);
      s.setNow(200);
      s.fail();
      assert.equal(await s.runtime.renewOwnerLeaseOnce(), false);
      const cutoff = retention ? 1300 : 1000;
      assert.equal(s.snapshot().storeOutage.startedAtMs, 200);
      assert.equal(s.snapshot().storeOutage.cutoffMs, cutoff);
      s.advance(cutoff - 1);
      assert.equal(s.snapshot().storeOutage.discarded, false);
      assert.equal(s.runtime.ownerLeaseUsable, retention ? false : true);
      s.advance(cutoff);
      assert.equal(s.snapshot().storeOutage.discarded, true);
      s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().storeOutage.discarded, true);
      assert.equal(s.runtime.ownerLeaseUsable, true); // Observation has no execution consumer yet.
    });
  }
  test(`successful renewal closes episode before T (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(300);
    s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage, undefined);
    s.advance(1300);
    assert.equal(s.snapshot().storeOutage, undefined);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1500);
  });
  test(`same owner renewal pending before outage closes it before T (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    let complete;
    s.setNow(150);
    s.lease.renewOwnerLease = (...args) =>
      new Promise((resolve) => {
        complete = async () => resolve(await s.store.renewOwnerLease(...args));
      });
    const pending = s.runtime.renewOwnerLeaseOnce();
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(300);
    await complete();
    await pending;
    assert.equal(s.snapshot().storeOutage, undefined);
    s.advance(1300);
    assert.equal(s.snapshot().storeOutage, undefined);
  });
  test(`new claim and old pending renewal cannot move original T (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    let complete;
    s.setNow(150);
    s.lease.renewOwnerLease = (...args) =>
      new Promise((resolve) => {
        complete = async () => resolve(await s.store.renewOwnerLease(...args));
      });
    const pending = s.runtime.renewOwnerLeaseOnce();
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(300);
    s.lease.renewOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    s.lease.claimOwnerLease = async () => ({
      kind: 'claimed',
      token: { ownerId: 'owner', leaseGeneration: 2n },
      storeNow: new Date(0),
      leaseExpiresAt: new Date(1000)
    });
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    s.setNow(1300);
    await complete();
    await pending;
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    assert.equal(s.snapshot().storeOutage.discarded, true);
  });
  test(`definite Missing and Conflict do not open an outage (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.lease.renewOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    s.lease.claimOwnerLease = async () => ({ kind: 'conflict', storeNow: new Date(0) });
    s.lease.readOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage, undefined);
  });
  test(`renewal response at T latches before scheduled callback (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.setNow(200);
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    s.setNow(1300);
    s.lease.renewOwnerLease = (...args) => s.store.renewOwnerLease(...args);
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.discarded, true);
  });
  test(`confirmation read provider error opens outage using original lease (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    s.setNow(200);
    s.lease.renewOwnerLease = async () => ({ kind: 'missing', storeNow: new Date(0) });
    s.lease.claimOwnerLease = async () => ({ kind: 'conflict', storeNow: new Date(0) });
    s.lease.readOwnerLease = async () => {
      throw new Error('read unavailable');
    };
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.startedAtMs, 200);
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
  });
  for (const retention of [false, true]) {
    test(`cutoff uses successful request start despite response delay (provider=${provider}, retention=${retention})`, async (t) => {
      const s = await scenario(t, provider, retention);
      s.setNow(200);
      s.lease.renewOwnerLease = async (...args) => {
        const result = await s.store.renewOwnerLease(...args);
        s.setNow(250);
        return result;
      };
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().lastSuccessfulLeaseStartedAtMs, 200);
      s.setNow(400);
      s.fail();
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().storeOutage.cutoffMs, retention ? 1400 : 1100);
      s.setNow(500);
      await s.runtime.renewOwnerLeaseOnce();
      assert.equal(s.snapshot().storeOutage.startedAtMs, 400);
      assert.equal(s.snapshot().storeOutage.cutoffMs, retention ? 1400 : 1100);
    });
  }
  test(`renew timeout opens episode with injected time (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider, true);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    s.setNow(200);
    const response = await s.store.renewOwnerLease(s.runtime.currentOwnerToken, 1000);
    let complete;
    s.lease.renewOwnerLease = () =>
      new Promise((resolve) => {
        complete = () => resolve(response);
      });
    const renewal = s.runtime.renewOwnerLeaseOnce();
    s.setNow(300);
    t.mock.timers.tick(3000);
    assert.equal(await renewal, false);
    assert.equal(s.snapshot().storeOutage.startedAtMs, 300);
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    s.setNow(400);
    await complete();
    await Promise.resolve();
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1300);
    assert.equal(s.snapshot().storeOutage.discarded, false);
  });
  test(`runtime copies startup retention option (provider=${provider})`, async (t) => {
    const s = await scenario(t, provider);
    s.options.storeOutageRetention = true;
    s.fail();
    await s.runtime.renewOwnerLeaseOnce();
    assert.equal(s.snapshot().storeOutage.cutoffMs, 1000);
  });
}

test('retention option defaults false, builds and validates boolean values', () => {
  assert.equal(zlinkDefaultLocationOptions.storeOutageRetention, false);
  const registration = internal.createFrameworkRegistrationWithBuilder((builder) => {
    builder.configureLocations().storeOutageRetention(true);
  });
  assert.equal(registration.locations.options.storeOutageRetention, true);
  assert.throws(
    () =>
      internal.createFrameworkRegistrationWithBuilder((builder) => {
        builder.configureLocations().storeOutageRetention('true');
      }),
    /storeOutageRetention.*boolean/
  );
});
