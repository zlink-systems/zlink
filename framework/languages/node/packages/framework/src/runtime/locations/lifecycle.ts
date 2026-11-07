import type { ActorRef, RoutingId } from '../../contracts/Common';
import { decodeAuthorityKey } from './authority-key-codec';
import { decodeActorAuthorityIdentity } from '../actors/actor-authority-publication';
import { ZLinkSpotKind } from '../../contracts/Spots';
import { routingIdsEqual } from '../routing-id';
import { rewriteActorAuthorityOwner } from '../actors/actor-authority-publication';
import { rewriteServiceAuthorityOwner } from '../foundation/service-authority-payload-codec';
import { isIndeterminateLocationStoreFailure } from './location-store-failure';
import {
  ZLinkActorLocationClaims,
  type ZLinkActorClaimActivation,
  type ZLinkActorClaimResult
} from './actor-location-claims';
import { ZLinkActorSessionRouteClaims } from './actor-session-route-claims';
import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkActorLocation,
  ZLinkLocationWriteStatus
} from './internal-location-contracts';
import type {
  ZLinkActorLocationStore,
  ZLinkAuthorityStore,
  ZLinkSpotLocationStore
} from './internal-store-contracts';
import type {
  ZLinkExecutionRecovery,
  IZLinkLocationLifecycleRuntime,
  ZLinkOwnershipLostEvent
} from './lifecycle-runtime';
import {
  type ZLinkInstanceClosingAuthority,
  type ZLinkTrackedInstanceAuthority,
  ZLinkSpotLocationClaims
} from './spot-location-claims';
export {
  ZLinkActorClaimStatus,
  type ZLinkActorClaimActivation,
  type ZLinkActorClaimResult
} from './actor-location-claims';
export type { IZLinkLocationLifecycleRuntime, ZLinkOwnershipLostEvent } from './lifecycle-runtime';

export class ZLinkLocationLifecycle {
  private readonly actorClaims: ZLinkActorLocationClaims;
  private readonly spotClaims: ZLinkSpotLocationClaims;
  private readonly actorSessionRoutes: ZLinkActorSessionRouteClaims;
  private readonly ownershipLostHandler = (event: ZLinkOwnershipLostEvent) =>
    this.onOwnershipLost(event);

  constructor(
    private readonly runtime: IZLinkLocationLifecycleRuntime,
    actorStore: ZLinkActorLocationStore,
    entryMeshName = '',
    private readonly authorityStore?: ZLinkAuthorityStore,
    spotStore?: ZLinkSpotLocationStore,
    invalidateSpotRoute?: (spotId: RoutingId) => void
  ) {
    this.actorClaims = new ZLinkActorLocationClaims(
      runtime,
      actorStore,
      entryMeshName,
      authorityStore
    );
    this.spotClaims = new ZLinkSpotLocationClaims(
      runtime,
      authorityStore,
      spotStore ?? inferSpotStore(actorStore),
      invalidateSpotRoute
    );
    this.actorSessionRoutes = new ZLinkActorSessionRouteClaims(runtime);
    this.runtime.addOwnershipLostHandler(this.ownershipLostHandler);
  }

  bindActorExecution(actorId: string, owner: object): void {
    this.actorClaims.bindExecution(actorId, owner);
  }

  bindSpotExecution(meshName: string, spotId: RoutingId, owner: object): void {
    this.spotClaims.bindExecution(meshName, spotId, owner);
  }

  trackExecution(owner: object, steadyReady: () => boolean, discard: () => void): void {
    this.runtime.trackExecution?.(owner, steadyReady, discard, (reference) =>
      this.captureRecovery(reference)
    );
  }

  forgetExecution(owner: object): void {
    this.runtime.forgetExecution?.(owner);
  }

  discardSteadyExecution(owner: object): boolean {
    return this.runtime.discardSteadyExecution?.(owner) ?? false;
  }

  discardExecution(owner: object): void {
    this.runtime.discardExecution?.(owner);
  }

  dispose(): void {
    this.runtime.removeOwnershipLostHandler(this.ownershipLostHandler);
  }

  async executeActorClaimThenActivate<TActor>(
    actorType: string,
    actorId: string,
    nodeRid: RoutingId,
    deactivate: (() => Promise<void>) | undefined,
    activate: () => Promise<TActor>
  ): Promise<ZLinkActorClaimActivation<TActor>> {
    return await this.actorClaims.executeClaimThenActivate(
      actorType,
      actorId,
      nodeRid,
      deactivate,
      activate
    );
  }

  async claimActor(
    actorType: string,
    actorId: string,
    nodeRid: RoutingId,
    deactivate?: () => Promise<void>
  ): Promise<ZLinkActorClaimResult> {
    return await this.actorClaims.claim(actorType, actorId, nodeRid, deactivate);
  }

  async setActorRef(
    actorType: string,
    actorId: string,
    actorRef: ActorRef,
    ownerNodeGeneration = 0n
  ): Promise<void> {
    await this.actorClaims.setRef(actorType, actorId, actorRef, ownerNodeGeneration);
  }

  async takeoverActorJoinedSpot(
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
    return await this.actorClaims.takeoverJoinedSpot(
      actorType,
      actorId,
      actorRef,
      spotMeshName,
      spotId,
      spotGeneration,
      membershipEpoch,
      ownerNodeGeneration,
      deactivate
    );
  }

  async commitActorMembership(
    actorType: string,
    actorRef: ActorRef,
    authorityOwnerGeneration: bigint,
    spotId: RoutingId,
    spotGeneration: bigint,
    spotKind: ZLinkSpotKind.Entry | ZLinkSpotKind.User,
    ownerNodeGeneration: bigint,
    signal?: AbortSignal
  ): Promise<void> {
    await this.actorClaims.commitMembership(
      actorType,
      actorRef,
      authorityOwnerGeneration,
      spotId,
      spotGeneration,
      spotKind,
      ownerNodeGeneration,
      signal
    );
  }

  async releaseActor(actorType: string, actorId: string, actorRef?: ActorRef): Promise<void> {
    await this.actorClaims.release(actorType, actorId, actorRef);
  }

  ownsActor(actorType: string, actorId: string): boolean {
    return this.actorClaims.owns(actorType, actorId);
  }

  actorLocationSnapshot(actorId: string): ZLinkActorLocation | undefined {
    return this.actorClaims.snapshot(actorId);
  }

  async claimSpot(
    meshName: string,
    spotId: RoutingId,
    spotType: string,
    nodeRid: RoutingId,
    spotKind: ZLinkSpotKind,
    spotGeneration: bigint,
    ownerNodeGeneration: bigint,
    deactivate?: () => Promise<void>
  ): Promise<ZLinkLocationWriteStatus> {
    return await this.spotClaims.claim(
      meshName,
      spotId,
      spotType,
      nodeRid,
      spotKind,
      spotGeneration,
      ownerNodeGeneration,
      deactivate
    );
  }

  async releaseSpot(
    meshName: string,
    spotId: RoutingId,
    expectedObjectGeneration?: bigint
  ): Promise<void> {
    await this.spotClaims.release(meshName, spotId, expectedObjectGeneration);
  }

  async beginInstanceSpotClosing(
    meshName: string,
    spotId: RoutingId,
    onCommitted: () => void
  ): Promise<ZLinkInstanceClosingAuthority | undefined> {
    return await this.spotClaims.beginInstanceClosing(meshName, spotId, onCommitted);
  }

  trackInstanceSpot(input: ZLinkTrackedInstanceAuthority): void {
    this.spotClaims.trackInstanceAuthority(input);
  }

  async reclaimOwnerRows(publication?: Promise<void>, signal?: AbortSignal): Promise<void> {
    const excluded = await this.runtime.reclaimOwnerAuthorities?.(
      publication,
      signal,
      (key, expected, confirmed) => this.confirmAuthority(key, confirmed, expected)
    );
    await this.actorClaims.reclaimOwnerRows(excluded);
    await this.spotClaims.reclaimOwnerRows(excluded);
  }

  confirmAuthority(
    key: ZLinkAuthorityKey,
    authority: ZLinkAuthoritySnapshot,
    expected?: ZLinkRecoveryAuthorityFence
  ): void {
    if (expected !== undefined && !sameRecoveryIncarnation(authority, expected)) return;
    const kind = authority.allocation.objectKind;
    if (kind === 'actor') {
      const identity = decodeActorAuthorityIdentity(authority.payload, authority.objectGeneration);
      if (identity !== undefined)
        this.actorClaims.confirmAuthority(
          identity.actor.actorId,
          authority,
          expected?.storeVersion.value
        );
    } else {
      const identity = decodeAuthorityKey(key);
      this.spotClaims.confirmAuthority(
        authority.allocation.descriptor.meshName,
        identity.globalId as unknown as RoutingId,
        authority,
        expected?.storeVersion.value
      );
    }
  }

  private captureRecovery(reference: WeakRef<object>): ZLinkExecutionRecovery | undefined {
    const execution = reference.deref();
    if (execution === undefined) return undefined;
    const tracked =
      this.actorClaims.recoveryAuthority(execution) ?? this.spotClaims.recoveryAuthority(execution);
    const key = tracked?.key;
    const snapshot = tracked?.fence;
    const fence =
      snapshot === undefined
        ? undefined
        : {
            storeVersion: snapshot.storeVersion,
            ownerId: snapshot.ownerId,
            ownerLeaseGeneration: snapshot.ownerLeaseGeneration,
            objectGeneration: snapshot.objectGeneration,
            authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
            allocation: snapshot.allocation
          };
    const store = this.authorityStore;
    if (key === undefined || fence === undefined || store === undefined) return undefined;
    let submission:
      | {
          readonly owner: Parameters<ZLinkExecutionRecovery['run']>[0];
          readonly operation: ReturnType<ZLinkAuthorityStore['compareExchangeAuthority']>;
        }
      | undefined;
    return {
      key,
      meshName: fence.allocation.descriptor.meshName,
      incarnation: fence,
      run: async (owner, canSubmit, signal) => {
        const current = await store.readAuthority(key, signal);
        const original =
          current.kind === 'snapshot' &&
          sameRecoveryIncarnation(current, fence) &&
          current.storeVersion.value === fence.storeVersion.value &&
          current.ownerLeaseGeneration === fence.ownerLeaseGeneration;
        if (submission === undefined) {
          if (!original || owner.ownerId !== fence.ownerId) return false;
          if (owner.leaseGeneration === fence.ownerLeaseGeneration) return true;
          const payload =
            current.allocation.objectKind === 'actor'
              ? rewriteActorAuthorityOwner(current.payload, owner)
              : rewriteServiceAuthorityOwner(current.payload, owner);
          if (payload === undefined || !canSubmit()) return false;
          submission = {
            owner,
            operation: store.compareExchangeAuthority(
              key,
              fence.storeVersion,
              {
                kind: 'rebindOwnerLease',
                expectedOwner: {
                  ownerId: fence.ownerId,
                  leaseGeneration: fence.ownerLeaseGeneration
                },
                targetOwner: owner,
                payload
              },
              signal
            )
          };
        }
        const submitted = submission;
        if (
          owner.ownerId !== submitted.owner.ownerId ||
          owner.leaseGeneration !== submitted.owner.leaseGeneration
        )
          return false;
        let confirmed: Awaited<ReturnType<ZLinkAuthorityStore['readAuthority']>>;
        let failure: unknown;
        try {
          if (original) {
            const result = await submitted.operation;
            if (result.kind !== 'stored') return false;
            confirmed = { ...result, kind: 'snapshot' };
          } else confirmed = current;
        } catch (error) {
          if (!isIndeterminateLocationStoreFailure(error, signal)) throw error;
          failure = error;
          confirmed = await store.readAuthority(key, signal);
        }
        if (
          confirmed.kind !== 'snapshot' ||
          !sameRecoveryIncarnation(confirmed, fence) ||
          confirmed.ownerLeaseGeneration !== submitted.owner.leaseGeneration
        ) {
          if (failure !== undefined) throw failure;
          return false;
        }
        const currentExecution = reference.deref();
        const currentTracked =
          currentExecution === undefined
            ? undefined
            : (this.actorClaims.recoveryAuthority(currentExecution) ??
              this.spotClaims.recoveryAuthority(currentExecution));
        if (
          currentTracked === undefined ||
          currentTracked.key.value !== key.value ||
          !sameRecoveryIncarnation(confirmed, currentTracked.fence)
        )
          return false;
        if (currentTracked.fence.ownerLeaseGeneration === submitted.owner.leaseGeneration)
          return true;
        if (currentTracked.fence.storeVersion.value !== fence.storeVersion.value) return false;
        this.confirmAuthority(key, confirmed, fence);
        return true;
      }
    };
  }

  async bindActorSessionRoute(
    sessionRid: RoutingId,
    actorId: string,
    ownerNodeRid: RoutingId
  ): Promise<void> {
    await this.actorSessionRoutes.bind(sessionRid, actorId, ownerNodeRid);
  }

  async removeActorSessionRoute(sessionRid: RoutingId): Promise<void> {
    await this.actorSessionRoutes.remove(sessionRid);
  }

  private onOwnershipLost(event: ZLinkOwnershipLostEvent): void {
    this.actorClaims.onOwnershipLost(event);
    this.spotClaims.onOwnershipLost(event);
    this.actorSessionRoutes.onOwnershipLost(event);
  }
}

function inferSpotStore(actorStore: ZLinkActorLocationStore): ZLinkSpotLocationStore | undefined {
  const candidate = actorStore as Partial<ZLinkSpotLocationStore>;
  if (
    typeof candidate.updateSpot === 'function' &&
    typeof candidate.removeSpot === 'function' &&
    typeof candidate.resolveSpot === 'function'
  ) {
    return actorStore as unknown as ZLinkSpotLocationStore;
  }
  return undefined;
}

export type ZLinkRecoveryAuthorityFence = Pick<
  ZLinkAuthoritySnapshot,
  | 'storeVersion'
  | 'ownerId'
  | 'ownerLeaseGeneration'
  | 'objectGeneration'
  | 'authorityOwnerGeneration'
> & {
  readonly allocation: Pick<
    ZLinkAuthoritySnapshot['allocation'],
    'descriptor' | 'descriptorLifecycleGeneration'
  >;
};

function sameRecoveryIncarnation(
  current: ZLinkAuthoritySnapshot,
  fence: ZLinkRecoveryAuthorityFence
): boolean {
  return (
    current.allocation.state === 'active' &&
    current.ownerId === fence.ownerId &&
    current.objectGeneration === fence.objectGeneration &&
    current.authorityOwnerGeneration === fence.authorityOwnerGeneration &&
    current.allocation.descriptor.meshName === fence.allocation.descriptor.meshName &&
    routingIdsEqual(current.allocation.descriptor.rid, fence.allocation.descriptor.rid) &&
    current.allocation.descriptorLifecycleGeneration ===
      fence.allocation.descriptorLifecycleGeneration
  );
}
