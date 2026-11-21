import type { RoutingId } from '../../contracts/Common';
import { ZLinkSpotKind } from '../../contracts';
import type { ZLinkFrameworkRuntimeState, ZLinkMeshNodeDescriptor } from '../../contracts';

export interface ZLinkSpotRouteResolver {
  resolve(
    spotId: RoutingId,
    signal: AbortSignal | undefined,
    instanceIntent: true
  ): Promise<ZLinkSpotRouteTarget | ZLinkInstanceReactivationRoute>;
  resolve(spotId: RoutingId, signal?: AbortSignal): Promise<ZLinkSpotRouteTarget>;
  invalidate?(spotId: RoutingId): void;
}

export interface ZLinkSpotRouteTarget {
  readonly authorityState?: 'ready' | 'closing';
  readonly routerChannelId: string;
  readonly targetNodeRid: RoutingId;
  readonly spotId: RoutingId;
  readonly spotKind: ZLinkSpotKind;
  /** Stable registered type for authority-backed User and Instance Spots. */
  readonly stableType?: string;
  /** Required for user Spot operations and absent for an Entry Spot route. */
  readonly targetSpotGeneration?: bigint;
  /** Authority fence used to install the resolved route in the stateful runtime. */
  readonly targetNodeGeneration?: bigint;
  /** Authority fence used to reject a superseded Spot owner. */
  readonly authorityOwnerGeneration?: bigint;
  /** Internal owner identity paired with ownerLeaseGeneration. */
  readonly targetOwnerId?: string;
  /** Exact owner lease generation paired with the authority route. */
  readonly ownerLeaseGeneration?: bigint;
  /** Store version paired with the authority fence. */
  readonly authorityStoreVersion?: string;
  /** Current state of the owning MeshNode, when the route was resolved from Location Store. */
  readonly targetNodeState?: ZLinkFrameworkRuntimeState;
}

export function entrySpotRouteTarget(
  routerChannelId: string,
  descriptor: ZLinkMeshNodeDescriptor
): ZLinkSpotRouteTarget | undefined {
  if (descriptor.entrySpotId === undefined) return undefined;
  return {
    routerChannelId,
    targetNodeRid: descriptor.rid,
    spotId: descriptor.entrySpotId as RoutingId,
    spotKind: ZLinkSpotKind.Entry,
    targetSpotGeneration: descriptor.lifecycleGeneration,
    targetNodeGeneration: descriptor.lifecycleGeneration,
    authorityOwnerGeneration: descriptor.lifecycleGeneration,
    targetOwnerId: descriptor.ownerId,
    ownerLeaseGeneration: descriptor.leaseGeneration,
    targetNodeState: descriptor.state
  };
}

export interface ZLinkInstanceReactivationRoute extends Pick<
  ZLinkSpotRouteTarget,
  'spotId' | 'spotKind' | 'stableType'
> {
  readonly kind: 'coldActivation';
  readonly meshName: string;
  readonly stableType: string;
}
