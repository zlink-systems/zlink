import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts';
import { ZLinkInMemoryLocationStore } from '../../packages/framework/src/runtime/locations/in-memory-location-store';
import { ZLinkInMemoryProviderLocationStore } from '../../packages/framework/src/runtime/locations/in-memory-provider-location-store';
import { ZLinkLocationStoreRepository } from '../../packages/framework/src/runtime/locations/location-store-repository';
import { createActive, reserveRequest, writeTargetDescriptor } from './user-spot-authority-fixture';

for (const backend of ['provider', 'memory'] as const) {
  test(`owner token release wait shares across rows and separates generations: ${backend}`, async () => {
    const now = () => new Date(100);
    let monotonicMs = 0;
    const marginMs = 5_000;
    const provider = new ZLinkInMemoryProviderLocationStore(now);
    const repository = () =>
      backend === 'provider'
        ? new ZLinkLocationStoreRepository(provider, now, undefined, () => monotonicMs, marginMs)
        : new ZLinkInMemoryLocationStore(now, () => monotonicMs, marginMs);

    const store = repository();
    const target = await seed(store);
    const request = (id: string) => ({
      ...reserveRequest(id, target),
      actorRelocationPolicy: 'disabled' as const
    });
    const waiting = (observer: typeof store, id: string) =>
      assert.rejects(
        observer.reserve(request(id)),
        (error: unknown) =>
          error instanceof ZLinkFrameworkException &&
          error.kind === ZLinkFrameworkErrorKind.Unavailable
      );

    await waiting(store, 'first');
    monotonicMs = 14_999;
    await waiting(store, 'first');
    monotonicMs = 15_000;
    assert.equal((await store.reserve(request('first'))).kind, 'reserved');
    assert.equal((await store.reserve(request('second'))).kind, 'reserved');
    await waiting(store, 'new-generation');

    const restarted = repository();
    if (backend === 'memory') await seed(restarted);
    await waiting(restarted, 'untouched');
    monotonicMs = 30_000;
    assert.equal((await store.reserve(request('new-generation'))).kind, 'reserved');
    assert.equal((await restarted.reserve(request('untouched'))).kind, 'reserved');

    async function seed(observer: typeof store) {
      const publish = async (ownerId: string, rid: string) => {
        const descriptor = await writeTargetDescriptor(observer, ownerId, rid, 60_000, 4);
        return {
          meshName: descriptor.meshName,
          nodeRid: descriptor.rid,
          nodeLifecycleGeneration: descriptor.lifecycleGeneration,
          owner: { ownerId, leaseGeneration: descriptor.leaseGeneration }
        };
      };
      const old = await publish('release-owner', 'first-node');
      for (const id of ['first', 'second', 'untouched']) await createActive(observer, id, old);
      assert.equal(await observer.releaseOwnerLease(old.owner), 'released');

      const next = await publish(old.owner.ownerId, 'next-node');
      assert.equal(next.owner.leaseGeneration, old.owner.leaseGeneration + 1n);
      await createActive(observer, 'new-generation', next);
      assert.equal(await observer.releaseOwnerLease(next.owner), 'released');
      return publish('release-target', 'target-node');
    }
  });
}
