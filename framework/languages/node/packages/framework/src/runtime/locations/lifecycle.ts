import type { ActorRef, RoutingId } from '../../contracts/Common';
import type {
  ZLinkActorLocation,
  ZLinkLocationWriteStatus
} from './internal-location-contracts';
import type {
  ZLinkActorLocationStore,
  ZLinkSpotLocationStore
} from './internal-store-contracts';
import { ZLinkSpotKind } from '../../contracts/Spots';
import {
  ZLinkActorLocationClaims,
  type ZLinkActorClaimActivation,
  type ZLinkActorClaimResult
} from './actor-location-claims';
export {
  ZLinkActorClaimStatus,
  type ZLinkActorClaimActivation,
  type ZLinkActorClaimResult
} from './actor-location-claims';
import { ZLinkActorSessionRouteClaims } from './actor-session-route-claims';
import { ZLinkSpotLocationClaims } from './spot-location-claims';
import type { ZLinkTrackedInstanceAuthority } from './spot-location-claims';
import type { ZLinkAuthorityStore } from './internal-store-contracts';
import type {
  IZLinkLocationLifecycleRuntime,
  ZLinkOwnershipLostEvent
} from './lifecycle-runtime';
export type {
  IZLinkLocationLifecycleRuntime,
  ZLinkOwnershipLostEvent
} from './lifecycle-runtime';

export class ZLinkLocationLifecycle {
  private readonly actorClaims: ZLinkActorLocationClaims;
  private readonly actorCleanupTasks = new Map<string, Promise<void>>();
  private readonly spotClaims: ZLinkSpotLocationClaims;
  private readonly actorSessionRoutes: ZLinkActorSessionRouteClaims;
  private disposed = false;
  private readonly ownershipLostHandler = (event: ZLinkOwnershipLostEvent) => this.onOwnershipLost(event);

  constructor(
    private readonly runtime: IZLinkLocationLifecycleRuntime,
    actorStore: ZLinkActorLocationStore,
    entryMeshName = '',
    authorityStore?: ZLinkAuthorityStore,
    spotStore?: ZLinkSpotLocationStore,
    invalidateSpotRoute?: (spotId: RoutingId) => void
  ) {
    this.actorClaims = new ZLinkActorLocationClaims(runtime, actorStore, entryMeshName, authorityStore);
    this.spotClaims = new ZLinkSpotLocationClaims(
      runtime,
      authorityStore,
      spotStore ?? inferSpotStore(actorStore),
      invalidateSpotRoute
    );
    this.actorSessionRoutes = new ZLinkActorSessionRouteClaims(runtime);
    this.runtime.addOwnershipLostHandler(this.ownershipLostHandler);
  }

  dispose(): void {
    this.disposed = true;
    this.runtime.removeOwnershipLostHandler(this.ownershipLostHandler);
  }

  async executeActorClaimThenActivate<TActor>(
    actorType: string,
    actorId: string,
    nodeRid: RoutingId,
    deactivate: (() => Promise<void>) | undefined,
    activate: () => Promise<TActor>
  ): Promise<ZLinkActorClaimActivation<TActor>> {
    return await this.actorClaims.executeClaimThenActivate(actorType, actorId, nodeRid, deactivate, activate);
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

  async notifyActorJoinedSpot(
    actorType: string,
    actorId: string,
    spotMeshName: string,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    ownerNodeGeneration: bigint
  ): Promise<void> {
    await this.actorClaims.notifyJoinedSpot(
      actorType,
      actorId,
      spotMeshName,
      spotId,
      spotGeneration,
      membershipEpoch,
      ownerNodeGeneration
    );
  }

  async notifyActorLeftSpot(
    actorType: string,
    actorId: string,
    entrySpotId: RoutingId,
    entrySpotGeneration: bigint,
    membershipEpoch: bigint,
    ownerNodeGeneration: bigint
  ): Promise<void> {
    await this.actorClaims.notifyLeftSpot(
      actorType,
      actorId,
      entrySpotId,
      entrySpotGeneration,
      membershipEpoch,
      ownerNodeGeneration
    );
  }

  async releaseActor(actorType: string, actorId: string, actorRef?: ActorRef): Promise<void> {
    await this.actorClaims.release(actorType, actorId, actorRef);
  }

  releaseActorEventually(actorType: string, actorId: string): Promise<void> {
    const existing = this.actorCleanupTasks.get(actorId);
    if (existing !== undefined) {
      return existing;
    }
    const cleanup = this.retryActorRelease(actorType, actorId)
      .finally(() => this.actorCleanupTasks.delete(actorId));
    this.actorCleanupTasks.set(actorId, cleanup);
    return cleanup;
  }

  ownsActor(actorType: string, actorId: string): boolean {
    return this.actorClaims.owns(actorType, actorId);
  }

  actorLocationSnapshot(actorId: string): ZLinkActorLocation | undefined {
    return this.actorClaims.snapshot(actorId);
  }

  private async retryActorRelease(actorType: string, actorId: string): Promise<void> {
    let retryDelayMs = 50;
    while (!this.disposed) {
      try {
        await this.actorClaims.release(actorType, actorId);
        return;
      } catch {
        await waitForRetry(retryDelayMs);
        retryDelayMs = Math.min(retryDelayMs * 2, 1_000);
      }
    }
    return;
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

  async beginInstanceSpotClosing(meshName: string, spotId: RoutingId): Promise<boolean> {
    return await this.spotClaims.beginInstanceClosing(meshName, spotId);
  }

  trackInstanceSpot(input: ZLinkTrackedInstanceAuthority): void {
    this.spotClaims.trackInstanceAuthority(input);
  }

  async reclaimOwnerRows(): Promise<void> {
    await this.runtime.reclaimOwnerAuthorities?.();
    await this.actorClaims.reclaimOwnerRows();
    await this.spotClaims.reclaimOwnerRows();
  }

  async bindActorSessionRoute(sessionRid: RoutingId, actorId: string, ownerNodeRid: RoutingId): Promise<void> {
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

function inferSpotStore(
  actorStore: ZLinkActorLocationStore
): ZLinkSpotLocationStore | undefined {
  const candidate = actorStore as Partial<ZLinkSpotLocationStore>;
  if (typeof candidate.updateSpot === 'function'
    && typeof candidate.removeSpot === 'function'
    && typeof candidate.resolveSpot === 'function') {
    return actorStore as unknown as ZLinkSpotLocationStore;
  }
  return undefined;
}

function waitForRetry(delayMs: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, delayMs));
}
