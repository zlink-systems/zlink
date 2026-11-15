import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '../../contracts/Errors';
import type { ActorRef, RoutingId } from '../../contracts/Common';
import {
  ZLinkLocationKind,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  type ZLinkActorLocation
} from './internal-location-contracts';
import type { ZLinkActorLocationStore, ZLinkAuthorityStore } from './internal-store-contracts';
import type { ZLinkAuthoritySnapshot } from './internal-location-contracts';
import type { ZLinkLocationOwnerToken } from '../../contracts/Locations/Writes';
import { ZLinkSpotKind } from '../../contracts/Spots';
import { ZLinkLocationKeyCodec } from './key-codec';
import { encodeAuthorityKey } from './authority-key-codec';
import { routingIdsEqual } from '../routing-id';
import {
  decodeActorAuthorityIdentity,
  rewriteActorAuthorityRoute
} from '../actors/actor-authority-publication';
import type { IZLinkLocationLifecycleRuntime, ZLinkOwnershipLostEvent } from './lifecycle-runtime';

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
  private readonly executionKeys = new WeakMap<object, string>();

  constructor(
    private readonly runtime: IZLinkLocationLifecycleRuntime,
    private readonly actorStore: ZLinkActorLocationStore,
    private readonly entryMeshName: string,
    private readonly authorityStore?: ZLinkAuthorityStore,
    private readonly observeActorAuthorityVersion?: (actorId: string, storeVersion: string) => void
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
      return {
        activated: await activate(),
        generation: claim.generation
      };
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
      spotGeneration:
        row.spotKind === ZLinkSpotKind.Entry ? ownerNodeGeneration : row.spotGeneration
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

  async commitMembership(
    actorType: string,
    actorRef: ActorRef,
    authorityOwnerGeneration: bigint,
    spotId: RoutingId,
    spotGeneration: bigint,
    spotKind: ZLinkSpotKind.Entry | ZLinkSpotKind.User,
    ownerNodeGeneration: bigint,
    signal?: AbortSignal
  ): Promise<void> {
    const actorId = actorRef.actorId;
    const store = this.authorityStore;
    const owner = this.runtime.currentOwnerToken;
    if (store === undefined || owner === undefined) {
      throw new Error('Actor membership commit requires Location Store authority and owner.');
    }
    const key = encodeAuthorityKey('actor', actorId);
    const current = await store.readAuthority(key, signal);
    if (current.kind === 'snapshot')
      this.observeActorAuthorityVersion?.(actorId, current.storeVersion.value);
    if (
      !matchesActorAuthority(current, actorRef, owner) ||
      current.allocation.stableType !== actorType ||
      current.authorityOwnerGeneration !== authorityOwnerGeneration ||
      current.allocation.descriptorLifecycleGeneration !== ownerNodeGeneration
    ) {
      throw new ZLinkFrameworkException(
        classifyMembershipAuthorityFailure(current, actorRef),
        `Actor '${actorId}' membership authority fence does not match.`
      );
    }
    const identity = decodeActorAuthorityIdentity(current.payload, current.objectGeneration);
    if (
      identity === undefined ||
      identity.actorType !== actorType ||
      identity.ownerNodeGeneration !== ownerNodeGeneration ||
      identity.owner.ownerId !== owner.ownerId ||
      identity.owner.leaseGeneration !== owner.leaseGeneration ||
      !routingIdsEqual(identity.actor.nodeRid, actorRef.nodeRid) ||
      identity.meshName !== actorRef.meshName
    ) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.Unavailable,
        `Actor '${actorId}' membership authority identity does not match.`
      );
    }
    const result = await store.compareExchangeAuthority(
      key,
      current.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        payload: rewriteActorAuthorityRoute(
          current.payload,
          actorRef,
          String(spotId),
          spotGeneration,
          spotKind
        )
      },
      signal
    );
    if (result.kind !== 'stored') {
      if (result.kind === 'conflict' && result.current.kind === 'snapshot')
        this.observeActorAuthorityVersion?.(actorId, result.current.storeVersion.value);
      throw new ZLinkFrameworkException(
        result.kind === 'conflict'
          ? (result.membershipFailure ??
              classifyMembershipAuthorityFailure(result.current, actorRef))
          : ZLinkFrameworkErrorKind.InternalFailure,
        `Actor '${actorId}' membership commit was rejected with status ${result.kind}.`
      );
    }
    this.confirmAuthority(actorId, { ...result, kind: 'snapshot' });
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
        this.forget(canonical);
      }
      if (status === ZLinkLocationWriteStatus.Stored) {
        await this.releaseAuthority(actorId, actorRef ?? tracked.row.actorRef, ownerToken);
      }
      return;
    }
    await this.releaseAuthority(actorId, actorRef, ownerToken);
  }

  owns(_actorType: string, actorId: string): boolean {
    return this.actors.has(
      ZLinkLocationKeyCodec.encodeActorKey({
        meshName: this.entryMeshName,
        actorId
      })
    );
  }

  snapshot(actorId: string): ZLinkActorLocation | undefined {
    const tracked = this.actors.get(
      ZLinkLocationKeyCodec.encodeActorKey({
        meshName: this.entryMeshName,
        actorId
      })
    );
    return tracked === undefined ? undefined : { ...tracked.row };
  }

  confirmAuthority(
    actorId: string,
    authority: ZLinkAuthoritySnapshot,
    expectedStoreVersion?: string
  ): void {
    const tracked = this.actors.get(
      ZLinkLocationKeyCodec.encodeActorKey({ meshName: this.entryMeshName, actorId })
    );
    if (
      expectedStoreVersion !== undefined &&
      tracked?.authority?.storeVersion.value !== expectedStoreVersion
    )
      return;
    if (tracked !== undefined) tracked.authority = authority;
    this.observeActorAuthorityVersion?.(actorId, authority.storeVersion.value);
  }

  recoveryAuthority(
    owner: object
  ): { key: ReturnType<typeof encodeAuthorityKey>; fence: ZLinkAuthoritySnapshot } | undefined {
    const canonical = this.executionKeys.get(owner);
    const tracked = canonical === undefined ? undefined : this.actors.get(canonical);
    if (tracked?.executionOwner?.deref() === owner && tracked.authority !== undefined)
      return {
        key: encodeAuthorityKey('actor', tracked.row.actorId),
        fence: tracked.authority
      };
    return undefined;
  }

  bindExecution(id: string, owner: object): void {
    const canonical = ZLinkLocationKeyCodec.encodeActorKey({
      meshName: this.entryMeshName,
      actorId: id
    });
    const tracked = this.actors.get(canonical);
    if (tracked !== undefined) {
      const previous = tracked.executionOwner?.deref();
      if (previous !== undefined) this.executionKeys.delete(previous);
      tracked.executionOwner = new WeakRef(owner);
      this.executionKeys.set(owner, canonical);
    }
  }

  private forget(canonical: string): void {
    const tracked = this.actors.get(canonical);
    const owner = tracked?.executionOwner?.deref();
    if (owner !== undefined) this.executionKeys.delete(owner);
    this.actors.delete(canonical);
  }

  onOwnershipLost(event: ZLinkOwnershipLostEvent): void {
    if (event.kind !== ZLinkLocationKind.Actor) {
      return;
    }
    const tracked = this.actors.get(event.key);
    this.forget(event.key);
    const executionOwner = tracked?.executionOwner?.deref();
    if (executionOwner !== undefined) this.runtime.discardExecution?.(executionOwner);
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
        leaseGeneration:
          this.runtime.currentOwnerToken?.leaseGeneration ?? candidate.leaseGeneration,
        updatedAt: result.updatedAt
      };
      tracked.generation = result.generation;
      return;
    }
    throw new Error(
      `Actor location renewal for '${actorId}' was rejected with status ${result.status}.`
    );
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

    let result = await store.compareExchangeAuthority(key, current.storeVersion, {
      kind: 'delete'
    });
    if (
      result.kind === 'conflict' &&
      result.current.kind === 'snapshot' &&
      matchesActorAuthority(result.current, actorRef, ownerToken)
    ) {
      result = await store.compareExchangeAuthority(key, result.current.storeVersion, {
        kind: 'delete'
      });
    }
    if (
      result.kind === 'deleted' ||
      (result.kind === 'conflict' && result.current.kind === 'missing')
    ) {
      return;
    }
    throw new ZLinkFrameworkException(
      result.kind === 'conflict'
        ? classifyMembershipAuthorityFailure(result.current, actorRef)
        : ZLinkFrameworkErrorKind.InternalFailure,
      `Actor '${actorId}' authority release was rejected with status ${result.kind}.`
    );
  }
}

interface TrackedActor {
  authority?: ZLinkAuthoritySnapshot;
  executionOwner?: WeakRef<object>;
  row: ZLinkActorLocation;
  generation: bigint;
  readonly deactivate?: () => Promise<void>;
}

function classifyMembershipAuthorityFailure(
  current: { readonly kind: 'missing' } | ZLinkAuthoritySnapshot,
  actorRef: ActorRef
): ZLinkFrameworkErrorKind {
  return current.kind === 'snapshot' && current.objectGeneration !== actorRef.objectGeneration
    ? ZLinkFrameworkErrorKind.InvalidOperation
    : ZLinkFrameworkErrorKind.Unavailable;
}

function matchesActorAuthority(
  current: { readonly kind: 'missing' } | ZLinkAuthoritySnapshot,
  actorRef: ActorRef,
  ownerToken: ZLinkLocationOwnerToken
): current is ZLinkAuthoritySnapshot {
  return (
    current.kind === 'snapshot' &&
    current.ownerId === ownerToken.ownerId &&
    current.ownerLeaseGeneration === ownerToken.leaseGeneration &&
    current.allocation.state === 'active' &&
    current.allocation.objectKind === 'actor' &&
    current.objectGeneration === actorRef.objectGeneration &&
    current.allocation.descriptor.meshName === actorRef.meshName &&
    routingIdsEqual(current.allocation.descriptor.rid, actorRef.nodeRid)
  );
}
