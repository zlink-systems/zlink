import assert from 'node:assert/strict';
import { test } from 'node:test';
import { RequestResult } from '@zlink-systems/zlink';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkSpotCreateState,
  ZLinkSpotKind
} from '../../packages/framework/src/contracts';
import type { ZLinkMeshNodeDescriptor } from '../../packages/framework/src/contracts/Locations';
import { ZLinkLocationWriteIntent } from '../../packages/framework/src/contracts/Locations/Writes';
import { publishInitialActorAuthority } from '../../packages/framework/src/runtime/actors/actor-authority-publication';
import { ZLinkActorPlacementCoordinator } from '../../packages/framework/src/runtime/host/actor-placement-coordinator';
import { ZLinkUserSpotCreationCoordinator } from '../../packages/framework/src/runtime/host/user-spot-creation-coordinator';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import { ZLinkInMemoryLocationStore } from '../../packages/framework/src/runtime/locations/in-memory-location-store';
import { ZLinkInMemoryProviderLocationStore } from '../../packages/framework/src/runtime/locations/in-memory-provider-location-store';
import { opaqueRecordPreimage } from '../../packages/framework/src/runtime/locations/opaque-record-key';
import { ZLinkOwnerLeaseTracker } from '../../packages/framework/src/runtime/locations/lease-tracker';
import { ZLinkLocationStoreRepository } from '../../packages/framework/src/runtime/locations/location-store-repository';
import { createActive, writeTargetDescriptor } from './user-spot-authority-fixture';

for (const backend of ['provider', 'memory'] as const) {
  for (const kind of ['actor', 'user_spot'] as const) {
    for (const policy of [
      'disabled',
      'requesterRecreate',
      'recreate',
      undefined,
      ...(backend === 'provider'
        ? (['weightChanged', 'policyEnabledChanged'] as const)
        : (['changedBeforeReserve'] as const))
    ] as const) {
      test(`creation reclaim uses selected target: ${backend}/${kind}/${policy}`, async () => {
        const now = () => new Date(100);
        let monotonicMs = 0;
        const provider = new ZLinkInMemoryProviderLocationStore(now);
        const store =
          backend === 'provider'
            ? new ZLinkLocationStoreRepository(provider, now, undefined, () => monotonicMs, 1)
            : new ZLinkInMemoryLocationStore(now, () => monotonicMs, 1);
        const old = await writeTargetDescriptor(store, 'old', 'old', 60_000);
        const target = await writeTargetDescriptor(store, 'target', 'target', 60_000);
        const stableType = 'room';
        const globalId = 'reclaim';
        const placement = (descriptor: ZLinkMeshNodeDescriptor) => ({
          meshName: descriptor.meshName,
          nodeRid: descriptor.rid,
          nodeLifecycleGeneration: descriptor.lifecycleGeneration,
          owner: { ownerId: descriptor.ownerId, leaseGeneration: descriptor.leaseGeneration }
        });
        const actor = (descriptor: typeof old, objectGeneration: bigint) => ({
          actorType: stableType,
          actor: {
            actorId: globalId,
            objectGeneration,
            meshName: descriptor.meshName,
            nodeRid: descriptor.rid
          },
          meshName: descriptor.meshName,
          ownerNodeGeneration: descriptor.lifecycleGeneration,
          owner: placement(descriptor).owner,
          spotId: 'entry',
          spotGeneration: descriptor.lifecycleGeneration,
          spotKind: ZLinkSpotKind.Entry as const
        });
        const initial =
          kind === 'actor'
            ? await publishInitialActorAuthority(store, actor(old, 1n))
            : await createActive(store, globalId, placement(old));
        const descriptor: ZLinkMeshNodeDescriptor = {
          ...target,
          objectCapabilities:
            policy === undefined
              ? [
                  { ...target.objectCapabilities[0]!, objectKind: kind, stableType: 'different' },
                  {
                    ...target.objectCapabilities[0]!,
                    objectKind: kind === 'actor' ? 'user_spot' : 'actor'
                  }
                ]
              : [
                  {
                    ...target.objectCapabilities[0]!,
                    objectKind: kind,
                    entryAccepted: kind === 'actor',
                    policy: policy === 'recreate' ? 'recreate' : 'disabled'
                  }
                ]
        };
        async function publish(row: ZLinkMeshNodeDescriptor) {
          await store.removeMeshNode(row, placement(row).owner);
          assert.equal(
            (await store.updateMeshNode(row, ZLinkLocationWriteIntent.NewClaim)).status,
            'stored'
          );
        }
        await publish(descriptor);
        const requester = await writeTargetDescriptor(store, 'requester', 'requester', 60_000);
        await publish({
          ...requester,
          objectCapabilities:
            policy === 'disabled'
              ? []
              : [
                  {
                    ...requester.objectCapabilities[0]!,
                    objectKind: kind,
                    policy: policy === 'requesterRecreate' ? 'recreate' : 'disabled'
                  }
                ]
        });
        assert.equal(await store.releaseOwnerLease(placement(old).owner), 'released');
        const key = encodeAuthorityKey(kind, globalId);
        const leaseTracker = new ZLinkOwnerLeaseTracker({ store });
        let factories = 0;
        let selections = 0;
        const select = async () => {
          selections++;
          if (policy === 'changedBeforeReserve') {
            await publish({ ...descriptor, objectCapabilities: [] });
          }
          return {
            ...placement(target),
            nodeGeneration: target.lifecycleGeneration,
            isLocal: false,
            entrySpotId: 'entry',
            sourceNodeRid: requester.rid,
            sourceNodeGeneration: 1n
          };
        };
        const coordinator: ZLinkActorPlacementCoordinator = new ZLinkActorPlacementCoordinator({
          store,
          leaseTracker,
          target: select,
          remoteCreate: async (_, rid, record) => {
            assert.equal(rid, target.rid);
            return coordinator.handleRemoteCreate(
              { kind: 'actorCreate', correlation: 1n, ...record },
              async (_, authority) => {
                factories++;
                return {
                  result: 'created',
                  actor: actor(target, authority.objectGeneration).actor,
                  entrySpotId: 'entry',
                  entrySpotGeneration: target.lifecycleGeneration
                };
              },
              new AbortController().signal
            );
          }
        });
        const targetCoordinator = new ZLinkUserSpotCreationCoordinator({
          store,
          leaseTracker,
          target: async () => undefined
        });
        const spotCoordinator = new ZLinkUserSpotCreationCoordinator({
          store,
          leaseTracker,
          target: select,
          remoteCreate: async (_, rid, record) => {
            assert.equal(rid, target.rid);
            const created = await targetCoordinator.handleRemoteCreate(
              {
                kind: 'userSpotCreate',
                correlation: 1n,
                operation: { high: 1n, low: 1n },
                ...record
              },
              async () => {
                factories++;
                return { spotId: globalId, state: ZLinkSpotCreateState.Created };
              }
            );
            return {
              terminalResult: RequestResult.Ok,
              failureCode: 0,
              tail: {
                kind: 'userSpotCreate',
                createResult: 'created',
                spotId: globalId,
                objectGeneration: created.spot.objectGeneration
              }
            };
          }
        });
        const create = () =>
          kind === 'actor'
            ? coordinator.create(
                globalId,
                stableType,
                policy === 'requesterRecreate',
                'mesh',
                Buffer.from('new'),
                1000
              )
            : spotCoordinator.getOrCreate(
                {
                  spotId: globalId,
                  stableType,
                  meshName: 'mesh',
                  requestPayload: Buffer.from('new'),
                  timeoutMs: 1000
                },
                async () => assert.fail('The requester does not run the remote factory.')
              );
        const unavailable = (error: unknown) =>
          error instanceof ZLinkFrameworkException &&
          error.kind === ZLinkFrameworkErrorKind.Unavailable;
        const recreates = policy === 'disabled' || policy === 'requesterRecreate';
        if (recreates || policy === 'weightChanged' || policy === 'policyEnabledChanged') {
          await assert.rejects(create, unavailable);
          monotonicMs = 3;
          selections = 0;
        }
        let rejectedBatch = false;
        if (policy === 'weightChanged' || policy === 'policyEnabledChanged') {
          const write = provider.write.bind(provider);
          provider.write = async (batch, signal) => {
            if (
              batch.mutations.some(
                (mutation) =>
                  mutation.kind === 'delete' &&
                  mutation.key.value ===
                    opaqueRecordPreimage('authority', kind === 'actor' ? 'actor' : 'spot', globalId)
              )
            ) {
              provider.write = write;
              if (policy === 'weightChanged') {
                await publish({
                  ...descriptor,
                  placementWeight: descriptor.placementWeight + 1
                });
              } else {
                await publish({
                  ...descriptor,
                  objectCapabilities: descriptor.objectCapabilities.map((capability) => ({
                    ...capability,
                    policy: 'recreate' as const
                  }))
                });
              }
              const result = await write(batch, signal);
              rejectedBatch = result.kind === 'conflict';
              return result;
            }
            return write(batch, signal);
          };
        }
        if (recreates || policy === 'weightChanged') {
          await create();
          if (policy === 'weightChanged') assert.ok(rejectedBatch);
          assert.equal(factories, 1);
          const ready = await store.readAuthority(key);
          assert.equal(ready.kind, 'snapshot');
          assert.ok(ready.kind === 'snapshot' && ready.objectGeneration > initial.objectGeneration);
        } else {
          await assert.rejects(create, unavailable);
          assert.equal(factories, 0);
          assert.deepEqual(await store.readAuthority(key), initial);
          if (policy === 'policyEnabledChanged') assert.ok(rejectedBatch);
        }
        assert.equal(selections, 1);
      });
    }
  }
}
