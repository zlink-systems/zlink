import type { ActorRef, RoutingId } from '../../contracts/Common';
import {
  ZLinkLocationKind,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkActorLocation
} from './internal-location-contracts';
import type { ZLinkActorLocationStore, ZLinkAuthorityStore } from './internal-store-contracts';
import type { ZLinkAuthoritySnapshot } from '../../contracts/Locations';
import type { ZLinkLocationOwnerToken } from '../../contracts/Locations/Writes';
import { ZLinkSpotKind } from '../../contracts/Spots';
import { ZLinkLocationKeyCodec } from './key-codec';
import { encodeAuthorityKey } from './authority-key-codec';
import { routingIdsEqual } from '../routing-id';
import type {
  IZLinkLocationLifecycleRuntime,
  ZLinkOwnershipLostEvent
} from './lifecycle-runtime';

export enum ZLinkActorClaimStatus {
  Claimed = 'claimed',
  AlreadyOwned = 'alreadyOwned',
  Conflict = 'conflict'
}

export interface ZLinkActorClaimResult {
  readonly status: ZLinkActorClaimStatus;
  readonly existing?: ZLinkActorLocation;
  readonly claimed?: ZLinkActorLocation;
  readonly generation?: bigint;
}

export interface ZLinkActorClaimActivation<TActor> {
  readonly activated?: TActor;
  readonly existingLocation?: ZLinkActorLocation;
  readonly generation?: bigint;
}

export class ZLinkActorLocationClaims {
  private readonly actors = new Map<string, TrackedActor>();

  constructor(
    private readonly runtime: IZLinkLocationLifecycleRuntime,
    private readonly actorStore: ZLinkActorLocationStore,
    private readonly entryMeshName: string,
    private readonly authorityStore?: ZLinkAuthorityStore
  ) {}

  async executeClaimThenActivate<TActor>(
    actorType: string,
    actorId: string,
    nodeRid: RoutingId,
    deactivate: (() => Promise<void>) | undefined,
    activate: () => Promise<TActor>
  ): Promise<ZLinkActorClaimActivation<TActor>> {
    const claim = await this.claim(actorType, actorId, nodeRid, deactivate);
    if (claim.status === ZLinkActorClaimStatus.AlreadyOwned) {
      return {};
    }
    if (claim.status === ZLinkActorClaimStatus.Conflict) {
      return { existingLocation: claim.existing };
    }
    try {
      return { activated: await activate(), generation: claim.generation };
    } catch (error) {
      await this.release(actorType, actorId);
      throw error;
    }
  }

  async claim(
    actorType: string,
    actorId: string,
    nodeRid: RoutingId,
    deactivate?: () => Promise<void>
  ): Promise<ZLinkActorClaimResult> {
    const normalizedType = ZLinkLocationKeyCodec.normalizeActorType(actorType);
    const key = { meshName: this.entryMeshName, actorId };
    const canonical = ZLinkLocationKeyCodec.encodeActorKey(key);
    if (this.actors.has(canonical)) {
      return { status: ZLinkActorClaimStatus.AlreadyOwned };
    }

    const row: ZLinkActorLocation = {
      meshName: this.entryMeshName,
      actorType: normalizedType,
      actorId,
      actorRef: {
        actorId,
        objectGeneration: 1n,
        meshName: this.entryMeshName,
        nodeRid
      },
      ownerNodeRid: nodeRid,
      ownerNodeGeneration: 0n,
      spotKind: ZLinkSpotKind.Entry,
      spotId: nodeRid,
      spotGeneration: 0n,
      membershipEpoch: 0n,
      ownerId: '',
      leaseGeneration: 0n,
      updatedAt: new Date(0)
    };
    const result = await this.runtime.writeActor(row, ZLinkLocationWriteIntent.NewClaim);
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      const claimed: ZLinkActorLocation = {
        ...row,
        ownerId: this.runtime.ownerId,
        leaseGeneration: this.runtime.currentOwnerToken?.leaseGeneration ?? row.leaseGeneration,
        updatedAt: result.updatedAt
      };
      this.actors.set(canonical, {
        row: claimed,
        generation: result.generation,
        deactivate
      });
      return { status: ZLinkActorClaimStatus.Claimed, claimed, generation: result.generation };
    }

    if (result.status === ZLinkLocationWriteStatus.RejectedConflict) {
      return {
        status: ZLinkActorClaimStatus.Conflict,
        existing: await this.actorStore.resolveActor(key)
      };
    }

    return { status: ZLinkActorClaimStatus.Conflict };
  }

  async setRef(
    actorType: string,
    actorId: string,
    actorRef: ActorRef,
    ownerNodeGeneration: bigint
  ): Promise<void> {
    await this.renew(actorType, actorId, (row) => ({
      ...row,
      actorRef,
      ownerNodeRid: actorRef.nodeRid,
      ownerNodeGeneration,
      spotId: row.spotKind === ZLinkSpotKind.Entry ? actorRef.nodeRid : row.spotId,
      spotGeneration: row.spotKind === ZLinkSpotKind.Entry ? ownerNodeGeneration : row.spotGeneration
    }));
  }

  async takeoverJoinedSpot(
    actorType: string,
    actorId: string,
    actorRef: ActorRef,
    spotMeshName: string,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    ownerNodeGeneration: bigint,
    deactivate?: () => Promise<void>
  ): Promise<ZLinkActorClaimResult> {
    const normalizedType = ZLinkLocationKeyCodec.normalizeActorType(actorType);
    const key = { meshName: spotMeshName, actorId };
    const canonical = ZLinkLocationKeyCodec.encodeActorKey(key);
    const row: ZLinkActorLocation = {
      meshName: spotMeshName,
      actorType: normalizedType,
      actorId,
      actorRef,
      ownerNodeRid: actorRef.nodeRid,
      ownerNodeGeneration,
      spotKind: ZLinkSpotKind.User,
      spotId,
      spotGeneration,
      membershipEpoch,
      ownerId: '',
      leaseGeneration: 0n,
      updatedAt: new Date(0)
    };
    let result = await this.runtime.writeActor(row, ZLinkLocationWriteIntent.Takeover);
    if (result.status !== ZLinkLocationWriteStatus.Stored) {
      const existing = await this.actorStore.resolveActor(key);
      if (existing === undefined) {
        result = await this.runtime.writeActor(row, ZLinkLocationWriteIntent.NewClaim);
      }
    }
    if (result.status !== ZLinkLocationWriteStatus.Stored) {
      return {
        status: ZLinkActorClaimStatus.Conflict,
        existing: await this.actorStore.resolveActor(key)
      };
    }
    const claimed: ZLinkActorLocation = {
      ...row,
      ownerId: this.runtime.ownerId,
      leaseGeneration: this.runtime.currentOwnerToken?.leaseGeneration ?? row.leaseGeneration,
      updatedAt: result.updatedAt
    };
    this.actors.set(canonical, {
      row: claimed,
      generation: result.generation,
      deactivate
    });
    return { status: ZLinkActorClaimStatus.Claimed, claimed, generation: result.generation };
  }

  async notifyJoinedSpot(
    actorType: string,
    actorId: string,
    _spotMeshName: string,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    ownerNodeGeneration: bigint
  ): Promise<void> {
    await this.renew(actorType, actorId, (row) => ({
      ...row,
      spotKind: ZLinkSpotKind.User,
      spotId,
      spotGeneration,
      membershipEpoch,
      ownerNodeGeneration
    }));
  }

  async notifyLeftSpot(
    actorType: string,
    actorId: string,
    entrySpotId: RoutingId,
    entrySpotGeneration: bigint,
    membershipEpoch: bigint,
    ownerNodeGeneration: bigint
  ): Promise<void> {
    await this.renew(actorType, actorId, (row) => ({
      ...row,
      spotKind: ZLinkSpotKind.Entry,
      spotId: entrySpotId,
      spotGeneration: entrySpotGeneration,
      membershipEpoch,
      ownerNodeGeneration
    }));
  }

  async release(_actorType: string, actorId: string, actorRef?: ActorRef): Promise<void> {
    const key = { meshName: this.entryMeshName, actorId };
    const canonical = ZLinkLocationKeyCodec.encodeActorKey(key);
    const tracked = this.actors.get(canonical);
    const ownerToken = this.runtime.currentOwnerToken;
    if (tracked !== undefined) {
      const status = await this.runtime.removeActor(
        key,
        ownerToken?.leaseGeneration ?? tracked.generation
      );
      if (this.actors.get(canonical) === tracked) {
        this.actors.delete(canonical);
      }
      if (status === ZLinkLocationWriteStatus.Stored) {
        await this.releaseAuthority(actorId, actorRef ?? tracked.row.actorRef, ownerToken);
      }
      return;
    }
    await this.releaseAuthority(actorId, actorRef, ownerToken);
  }

  owns(_actorType: string, actorId: string): boolean {
    return this.actors.has(ZLinkLocationKeyCodec.encodeActorKey({
      meshName: this.entryMeshName,
      actorId
    }));
  }

  snapshot(actorId: string): ZLinkActorLocation | undefined {
    const tracked = this.actors.get(ZLinkLocationKeyCodec.encodeActorKey({
      meshName: this.entryMeshName,
      actorId
    }));
    return tracked === undefined ? undefined : { ...tracked.row };
  }

  async reclaimOwnerRows(): Promise<void> {
    const owner = this.runtime.currentOwnerToken;
    if (owner === undefined) {
      throw new Error('Actor location recovery requires a claimed owner token.');
    }
    const failures: unknown[] = [];
    for (const [canonical, tracked] of [...this.actors]) {
      const key = { meshName: tracked.row.meshName, actorId: tracked.row.actorId };
      try {
        const current = await this.actorStore.resolveActor(key);
        if (current === undefined
          || current.ownerId !== owner.ownerId
          || (current.leaseGeneration !== tracked.row.leaseGeneration
            && current.leaseGeneration !== owner.leaseGeneration)) {
          this.actors.delete(canonical);
          await tracked.deactivate?.();
          continue;
        }
        if (current.leaseGeneration === owner.leaseGeneration) {
          tracked.row = { ...current };
          continue;
        }
        const result = await this.runtime.writeActor(
          current,
          ZLinkLocationWriteIntent.Takeover
        );
        if (result.status !== ZLinkLocationWriteStatus.Stored) {
          failures.push(new Error(
            `Actor location recovery for '${tracked.row.actorId}' was rejected with status ${result.status}.`
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
      } catch (error) {
        failures.push(error);
      }
    }
    if (failures.length > 0) {
      throw new AggregateError(failures, 'One or more Actor locations failed lease recovery.');
    }
  }

  onOwnershipLost(event: ZLinkOwnershipLostEvent): void {
    if (event.kind !== ZLinkLocationKind.Actor) {
      return;
    }
    const tracked = this.actors.get(event.key);
    this.actors.delete(event.key);
    if (tracked?.deactivate !== undefined) {
      void tracked.deactivate().catch(() => undefined);
    }
  }

  private async renew(
    _actorType: string,
    actorId: string,
    mutate: (row: ZLinkActorLocation) => ZLinkActorLocation
  ): Promise<void> {
    const canonical = ZLinkLocationKeyCodec.encodeActorKey({
      meshName: this.entryMeshName,
      actorId
    });
    const tracked = this.actors.get(canonical);
    if (tracked === undefined) {
      return;
    }
    const candidate = mutate(tracked.row);
    const result = await this.runtime.writeActor(candidate, ZLinkLocationWriteIntent.Renew);
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      tracked.row = {
        ...candidate,
        leaseGeneration: this.runtime.currentOwnerToken?.leaseGeneration
          ?? candidate.leaseGeneration,
        updatedAt: result.updatedAt
      };
      tracked.generation = result.generation;
      return;
    }
    throw new Error(`Actor location renewal for '${actorId}' was rejected with status ${result.status}.`);
  }

  private async releaseAuthority(
    actorId: string,
    actorRef: ActorRef | undefined,
    ownerToken: ZLinkLocationOwnerToken | undefined
  ): Promise<void> {
    const store = this.authorityStore;
    if (store === undefined || actorRef === undefined || ownerToken === undefined) return;

    const key = encodeAuthorityKey('actor', actorId);
    const current = await store.readAuthority(key);
    if (!matchesActorAuthority(current, actorRef, ownerToken)) return;

    const result = await store.compareExchangeAuthority(
      key,
      current.storeVersion,
      { kind: 'delete' }
    );
    if (result.kind === 'deleted' || (result.kind === 'conflict' && result.current.kind === 'missing')) {
      return;
    }
    if (result.kind === 'conflict' && result.current.kind === 'snapshot'
      && matchesActorAuthority(result.current, actorRef, ownerToken)) {
      const retry = await store.compareExchangeAuthority(
        key,
        result.current.storeVersion,
        { kind: 'delete' }
      );
      if (retry.kind === 'deleted' || (retry.kind === 'conflict' && retry.current.kind === 'missing')) {
        return;
      }
    }
  }
}

interface TrackedActor {
  row: ZLinkActorLocation;
  generation: bigint;
  readonly deactivate?: () => Promise<void>;
}

function matchesActorAuthority(
  current: { readonly kind: 'missing' } | ZLinkAuthoritySnapshot,
  actorRef: ActorRef,
  ownerToken: ZLinkLocationOwnerToken
): current is ZLinkAuthoritySnapshot {
  return current.kind === 'snapshot'
    && current.ownerId === ownerToken.ownerId
    && current.ownerLeaseGeneration === ownerToken.leaseGeneration
    && current.allocation.state === 'active'
    && current.allocation.objectKind === 'actor'
    && current.objectGeneration === actorRef.objectGeneration
    && current.allocation.descriptor.meshName === actorRef.meshName
    && routingIdsEqual(current.allocation.descriptor.rid, actorRef.nodeRid);
}
