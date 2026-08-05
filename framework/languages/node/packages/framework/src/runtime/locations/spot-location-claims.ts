import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type { RoutingId } from '../../contracts/Common';
import type {
  ZLinkAuthoritySnapshot,
  ZLinkAuthorityStoreVersion
} from '../../contracts/Locations';
import {
  ZLinkLocationKind,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkSpotLocation
} from './internal-location-contracts';
import { ZLinkSpotKind } from '../../contracts/Spots';
import { ZLinkLocationKeyCodec } from './key-codec';
import type {
  IZLinkLocationLifecycleRuntime,
  ZLinkOwnershipLostEvent
} from './lifecycle-runtime';
import type { ZLinkAuthorityStore, ZLinkSpotLocationStore } from './internal-store-contracts';
import { encodeAuthorityKey } from './authority-key-codec';
import { routingIdsEqual } from '../routing-id';
import {
  decodeServiceInstanceAuthorityPayload,
  encodeServiceInstanceAuthorityPayload
} from '../foundation/service-authority-payload-codec';

export class ZLinkSpotLocationClaims {
  private readonly spots = new Map<string, TrackedSpot>();

  constructor(
    private readonly runtime: IZLinkLocationLifecycleRuntime,
    private readonly authorityStore?: ZLinkAuthorityStore,
    private readonly spotStore?: ZLinkSpotLocationStore,
    private readonly invalidateSpotRoute?: (spotId: RoutingId) => void
  ) {}

  async claim(
    meshName: string,
    spotId: RoutingId,
    spotType: string,
    nodeRid: RoutingId,
    spotKind: ZLinkSpotKind,
    spotGeneration: bigint,
    ownerNodeGeneration: bigint,
    deactivate?: () => Promise<void>
  ): Promise<ZLinkLocationWriteStatus> {
    const row: ZLinkSpotLocation = {
      meshName,
      spotId,
      spotType,
      spotGeneration,
      ownerNodeRid: nodeRid,
      ownerNodeGeneration,
      spotKind,
      ownerId: '',
      leaseGeneration: 0n,
      updatedAt: new Date(0)
    };
    const result = await this.runtime.writeSpot(row, ZLinkLocationWriteIntent.NewClaim);
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      const owner = this.runtime.currentOwnerToken;
      this.spots.set(
        ZLinkLocationKeyCodec.encodeSpotKey({ meshName, spotId }),
        {
          kind: 'legacy',
          row: {
            ...row,
            ownerId: this.runtime.ownerId,
            leaseGeneration: owner?.leaseGeneration ?? row.leaseGeneration,
            updatedAt: result.updatedAt
          },
          generation: result.generation,
          deactivate
        }
      );
    }
    return result.status;
  }

  trackInstanceAuthority(input: ZLinkTrackedInstanceAuthority): void {
    if (this.authorityStore === undefined) {
      return;
    }
    this.spots.set(
      ZLinkLocationKeyCodec.encodeSpotKey({
        meshName: input.meshName,
        spotId: input.spotId
      }),
      { kind: 'authority', ...input }
    );
  }

  async release(
    meshName: string,
    spotId: RoutingId,
    expectedObjectGeneration?: bigint
  ): Promise<void> {
    const key = { meshName, spotId };
    const canonical = ZLinkLocationKeyCodec.encodeSpotKey(key);
    const tracked = this.spots.get(canonical);
    if (tracked === undefined) {
      return;
    }
    if (
      expectedObjectGeneration !== undefined
      && (tracked.kind !== 'authority' || tracked.objectGeneration !== expectedObjectGeneration)
    ) {
      return;
    }
    if (tracked.kind === 'legacy') {
      this.spots.delete(canonical);
      await this.runtime.removeSpot(
        key,
        this.runtime.currentOwnerToken?.leaseGeneration ?? tracked.generation
      );
      this.invalidateSpotRoute?.(tracked.row.spotId);
      return;
    }
    await this.releaseAuthority(tracked);
    if (this.spots.get(canonical) === tracked) {
      this.spots.delete(canonical);
    }
    this.invalidateSpotRoute?.(tracked.spotId);
  }

  async beginInstanceClosing(meshName: string, spotId: RoutingId): Promise<boolean> {
    const canonical = ZLinkLocationKeyCodec.encodeSpotKey({ meshName, spotId });
    const tracked = this.spots.get(canonical);
    const store = this.authorityStore;
    if (tracked?.kind !== 'authority' || store === undefined) {
      return false;
    }
    const key = encodeAuthorityKey('instance_spot', String(spotId));
    const current = await store.readAuthority(key);
    if (current.kind !== 'snapshot' || !matchesTrackedAuthority(current, tracked)) {
      return false;
    }
    const decoded = decodeServiceInstanceAuthorityPayload(current.payload);
    if (decoded?.state !== 'ready' || decoded.activationRecovery !== undefined) {
      return false;
    }
    const result = await store.compareExchangeAuthority(
      key,
      current.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        payload: encodeServiceInstanceAuthorityPayload({
          state: 'closing',
          stableType: decoded.stableType,
          spotId: decoded.spotId,
          ownerId: decoded.ownerId,
          ownerLeaseGeneration: decoded.ownerLeaseGeneration,
          ownerMeshName: decoded.ownerMeshName,
          ownerNodeRid: decoded.ownerNodeRid,
          ownerNodeGeneration: decoded.ownerNodeGeneration
        })
      }
    );
    if (result.kind !== 'stored') {
      return false;
    }
    tracked.storeVersion = result.storeVersion.value;
    this.invalidateSpotRoute?.(tracked.spotId);
    return true;
  }

  onOwnershipLost(event: ZLinkOwnershipLostEvent): void {
    if (event.kind !== ZLinkLocationKind.Spot) {
      return;
    }
    const tracked = this.spots.get(event.key);
    this.spots.delete(event.key);
    if (tracked !== undefined) {
      this.invalidateSpotRoute?.(
        tracked.kind === 'authority' ? tracked.spotId : tracked.row.spotId
      );
    }
    if (tracked?.deactivate !== undefined) {
      void tracked.deactivate().catch(() => undefined);
    }
  }

  async reclaimOwnerRows(): Promise<void> {
    const owner = this.runtime.currentOwnerToken;
    if (owner === undefined) {
      throw new Error('Spot location recovery requires a claimed owner token.');
    }
    const failures: unknown[] = [];
    for (const [canonical, tracked] of [...this.spots]) {
      try {
        if (tracked.kind === 'legacy') {
          const current = await this.spotStore?.resolveSpot({
            meshName: tracked.row.meshName,
            spotId: tracked.row.spotId
          });
          if (current === undefined
            || current.ownerId !== owner.ownerId
            || (current.leaseGeneration !== tracked.row.leaseGeneration
              && current.leaseGeneration !== owner.leaseGeneration)) {
            this.spots.delete(canonical);
            await tracked.deactivate?.();
            continue;
          }
          if (current.leaseGeneration === owner.leaseGeneration) {
            tracked.row = { ...current };
            continue;
          }
          const result = await this.runtime.writeSpot(
            current,
            ZLinkLocationWriteIntent.Takeover
          );
          if (result.status !== ZLinkLocationWriteStatus.Stored) {
            failures.push(new Error(
              `Spot location recovery for '${tracked.row.spotId}' was rejected with status ${result.status}.`
            ));
            continue;
          }
          tracked.row = {
            ...current,
            ownerId: owner.ownerId,
            leaseGeneration: owner.leaseGeneration,
            updatedAt: result.updatedAt
          };
          tracked.generation = result.generation;
          continue;
        }

        const key = encodeAuthorityKey('instance_spot', String(tracked.spotId));
        const current = await this.authorityStore?.readAuthority(key);
        if (current === undefined || current.kind === 'missing'
          || !matchesTrackedAuthorityIdentity(current, tracked)
          || current.ownerId !== owner.ownerId) {
          this.spots.delete(canonical);
          await tracked.deactivate?.();
          continue;
        }
        if (current.ownerLeaseGeneration !== owner.leaseGeneration) {
          const rebound = await this.authorityStore!.compareExchangeAuthority(
            key,
            current.storeVersion,
            {
              kind: 'rebindOwnerLease',
              expectedOwner: {
                ownerId: current.ownerId,
                leaseGeneration: current.ownerLeaseGeneration
              },
              targetOwner: owner,
              payload: Buffer.from(current.payload)
            }
          );
          if (rebound.kind !== 'stored') {
            failures.push(new Error(
              `Instance Spot '${String(tracked.spotId)}' authority recovery returned '${rebound.kind}'.`
            ));
            continue;
          }
          tracked.storeVersion = rebound.storeVersion.value;
          tracked.ownerLeaseGeneration = owner.leaseGeneration;
        } else {
          tracked.storeVersion = current.storeVersion.value;
        }
      } catch (error) {
        failures.push(error);
      }
    }
    if (failures.length > 0) {
      throw new AggregateError(failures, 'One or more Spot locations failed lease recovery.');
    }
  }

  private async releaseAuthority(tracked: TrackedAuthoritySpot): Promise<void> {
    const store = this.authorityStore;
    if (store === undefined) return;
    const key = encodeAuthorityKey('instance_spot', String(tracked.spotId));
    let result = await store.compareExchangeAuthority(
      key,
      { value: tracked.storeVersion } as ZLinkAuthorityStoreVersion,
      { kind: 'delete' }
    );
    if (
      result.kind === 'conflict'
      && result.current.kind === 'snapshot'
      && matchesTrackedAuthority(result.current, tracked)
    ) {
      result = await store.compareExchangeAuthority(
        key,
        result.current.storeVersion,
        { kind: 'delete' }
      );
    }
    if (
      result.kind === 'conflict'
      && result.current.kind === 'snapshot'
      && !matchesTrackedAuthority(result.current, tracked)
    ) {
      return;
    }
    if (result.kind === 'deleted' || (
      result.kind === 'conflict' && result.current.kind === 'missing'
    )) {
      return;
    }
    if (result.kind === 'generationExhausted') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `Instance Spot '${String(tracked.spotId)}' authority generation is exhausted.`
      );
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.InvalidOperation,
      `Instance Spot '${String(tracked.spotId)}' authority is owned by another runtime.`,
      true
    );
  }
}

export interface ZLinkTrackedInstanceAuthority {
  readonly meshName: string;
  readonly spotId: RoutingId;
  readonly stableType: string;
  readonly nodeRid: RoutingId;
  readonly nodeGeneration: bigint;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerId: string;
  ownerLeaseGeneration: bigint;
  storeVersion: string;
  readonly deactivate?: () => Promise<void>;
}

interface TrackedLegacySpot {
  readonly kind: 'legacy';
  row: ZLinkSpotLocation;
  generation: bigint;
  readonly deactivate?: () => Promise<void>;
}

interface TrackedAuthoritySpot extends ZLinkTrackedInstanceAuthority {
  readonly kind: 'authority';
}

type TrackedSpot = TrackedLegacySpot | TrackedAuthoritySpot;

function matchesTrackedAuthority(
  snapshot: ZLinkAuthoritySnapshot,
  tracked: TrackedAuthoritySpot
): boolean {
  return snapshot.objectGeneration === tracked.objectGeneration
    && snapshot.authorityOwnerGeneration === tracked.authorityOwnerGeneration
    && snapshot.ownerId === tracked.ownerId
    && snapshot.ownerLeaseGeneration === tracked.ownerLeaseGeneration
    && snapshot.allocation.state === 'active'
    && snapshot.allocation.objectKind === 'instance_spot'
    && snapshot.allocation.stableType === tracked.stableType
    && snapshot.allocation.descriptor.meshName === tracked.meshName
    && routingIdsEqual(snapshot.allocation.descriptor.rid, tracked.nodeRid)
    && snapshot.allocation.descriptorLifecycleGeneration === tracked.nodeGeneration;
}

function matchesTrackedAuthorityIdentity(
  snapshot: ZLinkAuthoritySnapshot,
  tracked: TrackedAuthoritySpot
): boolean {
  return snapshot.objectGeneration === tracked.objectGeneration
    && snapshot.authorityOwnerGeneration === tracked.authorityOwnerGeneration
    && snapshot.allocation.state === 'active'
    && snapshot.allocation.objectKind === 'instance_spot'
    && snapshot.allocation.stableType === tracked.stableType
    && snapshot.allocation.descriptor.meshName === tracked.meshName
    && routingIdsEqual(snapshot.allocation.descriptor.rid, tracked.nodeRid)
    && snapshot.allocation.descriptorLifecycleGeneration === tracked.nodeGeneration;
}
